"""Small synthesis toolkit for tools/audio_gen (numpy only): oscillators, envelopes, FFT filters, reverb, mixing, export.

Everything is vectorised: a note is a few array operations, filters are done in the frequency domain, the reverb is one FFT
convolution. Nothing here reads or downloads outside material.
"""
import os
import subprocess
import wave

import numpy as np

TAU = 2.0 * np.pi
SR = 44100  # music


def hz(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


# ------------------------------------------------------------------------------------------------ envelopes
def smoothstep(x):
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3.0 - 2.0 * x)


def ar_env(n, sr, attack, hold, release):
    """Rise over `attack` (smooth), stay until `hold` seconds, fall over `release` (cosine). n samples."""
    t = np.arange(n) / sr
    up = smoothstep(t / max(attack, 1e-4))
    down = np.where(t < hold, 1.0, 0.5 + 0.5 * np.cos(np.pi * np.clip((t - hold) / max(release, 1e-4), 0.0, 1.0)))
    return up * down


# ------------------------------------------------------------------------------------------------ wavetables
TAB_N = 4096
_tabs = {}


def table(kind, nh):
    """One cycle with nh harmonics (band-limited by the caller), peak-normalised."""
    key = (kind, nh)
    if key in _tabs:
        return _tabs[key]
    ph = np.arange(TAB_N + 1) / TAB_N
    w = np.zeros(TAB_N + 1)
    for h in range(1, nh + 1):
        if kind == "dark":
            a = 1.0 / h ** 2.1
        elif kind == "warm":
            a = (1.0 if h % 2 else 0.6) / h ** 1.8
        elif kind == "hollow":  # odd harmonics: clarinet-like, soft
            a = (1.0 / h ** 1.7) if h % 2 else 0.08 / h
        else:
            a = 1.0 / h ** 1.2
        w += a * np.sin(TAU * h * ph)
    w /= np.max(np.abs(w))
    _tabs[key] = w
    return w


def tab_osc(tab, phase):
    """phase in cycles (any real); linear interpolation into a wavetable."""
    p = (phase % 1.0) * TAB_N
    return np.interp(p, np.arange(TAB_N + 1), tab)


# ------------------------------------------------------------------------------------------------ FFT filtering
def fft_filter(x, sr, lo=None, hi=None, order=2, tilt_db_oct=0.0, circular=False):
    """Butterworth-shaped magnitude band (lo/hi in Hz) and an optional spectral tilt (dB per octave re 1 kHz).
    circular=True filters in place over len(x) (the result loops seamlessly); else zero-padded (no wrap)."""
    n = len(x)
    N = n if circular else 1 << int(np.ceil(np.log2(n * 2)))
    X = np.fft.rfft(x, N)
    f = np.fft.rfftfreq(N, 1.0 / sr)
    f[0] = 1e-3
    g = np.ones_like(f)
    if lo:
        g *= 1.0 / np.sqrt(1.0 + (lo / f) ** (2 * order))
    if hi:
        g *= 1.0 / np.sqrt(1.0 + (f / hi) ** (2 * order))
    if tilt_db_oct:
        g *= 10.0 ** (tilt_db_oct * np.log2(f / 1000.0) / 20.0)
    g[0] = 0.0
    y = np.fft.irfft(X * g, N)
    return y[:n]


def circ_convolve(x, h):
    """Circular convolution of x with an impulse response h (len(h) <= len(x)): a loop stays a loop."""
    n = len(x)
    return np.fft.irfft(np.fft.rfft(x) * np.fft.rfft(h, n), n)


def lin_convolve(x, h):
    n = len(x) + len(h) - 1
    N = 1 << int(np.ceil(np.log2(n)))
    return np.fft.irfft(np.fft.rfft(x, N) * np.fft.rfft(h, N), N)[:n]


def smooth_random(n, sr, rng, rate_hz):
    """A slow, smooth random curve (std 1) that loops over n samples."""
    w = rng.standard_normal(n)
    y = fft_filter(w, sr, hi=rate_hz, order=2, circular=True)
    s = np.std(y)
    return y / s if s > 0 else y


def colored_noise(n, sr, rng, lo=None, hi=None, order=2, tilt=0.0):
    """Looping band noise, std 1."""
    y = fft_filter(rng.standard_normal(n), sr, lo=lo, hi=hi, order=order, tilt_db_oct=tilt, circular=True)
    return y / np.std(y)


# ------------------------------------------------------------------------------------------------ reverb
def make_ir(sr, rng, rt_low=3.0, rt_high=1.0, length=4.0, predelay=0.02, split=2200.0):
    """Stereo, decorrelated, darker as it decays (two bands with their own RT60). Unit energy per channel."""
    n = int(length * sr)
    t = np.arange(n) / sr
    out = []
    for _ in range(2):
        w = rng.standard_normal(n)
        low = fft_filter(w, sr, hi=split, order=2)
        high = w - low
        ir = low * np.exp(-6.9 * t / rt_low) + 0.6 * high * np.exp(-6.9 * t / rt_high)
        ir *= 1.0 - np.exp(-t / 0.012)  # no hard onset
        ir = np.concatenate([np.zeros(int(predelay * sr)), ir])
        out.append(ir / np.sqrt(np.sum(ir ** 2)))
    return out


# ------------------------------------------------------------------------------------------------ mixing
class Bus:
    """A stereo buffer with a given length; notes are added at a time with a pan (-1..1, equal power)."""

    def __init__(self, n):
        self.L = np.zeros(n)
        self.R = np.zeros(n)
        self.n = n

    def add(self, sig, t0, sr, pan=0.0, gain=1.0, right=None, wrap=False):
        i0 = int(round(t0 * sr))
        a = (pan + 1.0) * np.pi / 4.0
        gl, gr = np.cos(a) * gain, np.sin(a) * gain
        left = sig
        rgt = sig if right is None else right
        self._add(self.L, left * gl, i0, wrap)
        self._add(self.R, rgt * gr, i0, wrap)

    def _add(self, buf, s, i0, wrap):
        n = self.n
        if wrap:
            i0 %= n
            pos = 0
            while pos < len(s):
                k = min(len(s) - pos, n - i0)
                buf[i0:i0 + k] += s[pos:pos + k]
                pos += k
                i0 = 0
            return
        if i0 >= n:
            return
        if i0 < 0:
            s = s[-i0:]
            i0 = 0
        k = min(len(s), n - i0)
        buf[i0:i0 + k] += s[:k]

    def mix(self, other, gain=1.0):
        self.L += other.L * gain
        self.R += other.R * gain


# ------------------------------------------------------------------------------------------------ export
def to_int16(x, rng):
    """float (-1..1) -> int16 with TPDF dither."""
    d = (rng.random(x.shape) - rng.random(x.shape)) / 32768.0
    return np.clip(np.round((x + d) * 32767.0), -32768, 32767).astype(np.int16)


def write_wav(path, chans, sr, rng):
    data = np.stack([to_int16(c, rng) for c in chans], axis=1) if len(chans) > 1 else to_int16(chans[0], rng)[:, None]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(data.shape[1])
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(data.tobytes())


def encode(wav_path, out_path, kind):
    """ffmpeg: kind 'mp3' (128 kb/s CBR) or 'flac' (16-bit). Removes the wav."""
    if kind == "mp3":
        codec = ["-c:a", "libmp3lame", "-b:a", "128k"]
    else:
        codec = ["-c:a", "flac", "-sample_fmt", "s16", "-compression_level", "8"]
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", wav_path, *codec, out_path], check=True)
    os.remove(wav_path)


# ------------------------------------------------------------------------------------------------ checks
def db(x):
    return 20.0 * np.log10(max(float(x), 1e-12))


def report(name, chans, sr, loop=False):
    """Peak, RMS, DC, the share of energy above 8 kHz, and for a loop the jump across the seam vs the typical step."""
    x = np.stack(chans)
    peak = np.max(np.abs(x))
    rms = np.sqrt(np.mean(x ** 2))
    dc = np.max(np.abs(np.mean(x, axis=1)))
    m = x.mean(axis=0)
    X = np.abs(np.fft.rfft(m)) ** 2
    f = np.fft.rfftfreq(len(m), 1.0 / sr)
    hf = X[f > 8000].sum() / max(X.sum(), 1e-20)
    line = "%-26s %6.1fs peak %6.1f dBFS  rms %6.1f dBFS  dc %.1e  >8k %5.2f%%" % (name, x.shape[1] / sr, db(peak), db(rms), dc, 100 * hf)
    ok = peak <= 0.9 and dc < max(1e-3, 0.02 * rms)  # a short one-shot's mean is a few % of its level, harmlessly
    if loop:
        steps = np.abs(np.diff(x, axis=1))
        typical = np.percentile(steps, 99.9)
        seam = np.max(np.abs(x[:, 0] - x[:, -1]))
        # the loudness just before and after the seam (100 ms each) against the whole: no level jump
        w = int(0.1 * sr)
        a = np.sqrt(np.mean(x[:, -w:] ** 2)) + 1e-12
        b = np.sqrt(np.mean(x[:, :w] ** 2)) + 1e-12
        line += "  seam %.4f (p99.9 step %.4f) level jump %+.1f dB" % (seam, typical, db(b / a))
        ok = ok and seam <= typical * 1.5 + 1e-4
    print(line + ("" if ok else "   <-- CHECK"))
    return ok
