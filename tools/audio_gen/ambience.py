"""The city view's ambience beds and the footsteps. Every bed is a seamless loop: noise is filtered over the loop's own
length in the frequency domain (circular), slow modulations are circular too, and events (a chirp, a car) that run past
the end wrap round to the start. The reverb on a bed is a circular convolution, so its tail also wraps.
"""
import numpy as np

from synth import TAU, Bus, circ_convolve, colored_noise, fft_filter, make_ir, smooth_random

SRA = 22050  # beds: nothing in them needs more than 11 kHz


def _norm(chans, rms_db):
    x = np.concatenate(chans)
    g = 10 ** (rms_db / 20) / (np.sqrt(np.mean(x ** 2)) + 1e-12)
    peak = max(np.max(np.abs(c)) for c in chans) * g
    if peak > 0.85:
        g *= 0.85 / peak
    return [c * g for c in chans]


def _verb(bus, rng, rt=1.6, wet=0.35, length=None):
    ir = make_ir(SRA, rng, rt_low=rt, rt_high=rt * 0.5, length=length or rt * 1.2, predelay=0.015)
    bus.L = bus.L + wet * circ_convolve(bus.L, ir[0])
    bus.R = bus.R + wet * circ_convolve(bus.R, ir[1])


def air(seconds, rng):
    """Wind and open air: a soft broadband bed with slow gusts, a faint higher whistle in the strong ones. Stereo."""
    n = int(seconds * SRA)
    gust = smooth_random(n, SRA, rng, 0.06)
    env = 0.55 + 0.45 * np.tanh(0.8 * gust)
    whistle_env = np.clip(gust - 0.6, 0, None) ** 1.5
    out = []
    for ch in range(2):
        body = colored_noise(n, SRA, rng, lo=60, hi=700, order=2, tilt=-3.0)
        hiss = colored_noise(n, SRA, rng, lo=900, hi=3200, order=2)
        sway = 1.0 + 0.15 * smooth_random(n, SRA, rng, 0.2)
        out.append(body * env * sway + 0.10 * hiss * env ** 2 + 0.05 * hiss * whistle_env)
    return _norm(out, -24)


def hum(seconds, rng):
    """A city far off: low rumble, a thin band of distant traffic, a faint mains-like drone that drifts. Mono."""
    n = int(seconds * SRA)
    t = np.arange(n) / SRA
    rumble = colored_noise(n, SRA, rng, lo=35, hi=220, order=2, tilt=-2.0)
    band = colored_noise(n, SRA, rng, lo=250, hi=900, order=2)
    swell = 0.8 + 0.2 * smooth_random(n, SRA, rng, 0.05)
    # drones at whole numbers of cycles over the loop so they wrap cleanly
    drone = np.zeros(n)
    for f, a in ((58.0, 0.12), (116.0, 0.06), (174.0, 0.025)):
        f = round(f * seconds) / seconds
        drone += a * np.sin(TAU * f * t + rng.random() * TAU) * (0.7 + 0.3 * smooth_random(n, SRA, rng, 0.1))
    return _norm([(rumble + 0.35 * band) * swell + drone], -24)


def traffic(seconds, rng, cars_per_min=22):
    """Cars passing a street away: tyre hiss and engine swelling and fading as each passes, panned across, over a road
    rumble. Stereo."""
    n = int(seconds * SRA)
    bus = Bus(n)
    k = int(cars_per_min * seconds / 60)
    times = np.sort(rng.random(k)) * seconds
    for tc in times:
        w = 0.6 + 1.4 * rng.random()  # pass-by width, seconds
        span = 5 * w
        m = int(2 * span * SRA)
        t = np.arange(m) / SRA - span
        prox = 1.0 / (1.0 + (t / w) ** 2)
        tyre = fft_filter(rng.standard_normal(m), SRA, lo=300 + 300 * rng.random(), hi=2600, order=2)
        tyre /= np.std(tyre) + 1e-9
        f0 = 45 + 40 * rng.random()
        dop = 1.0 + 0.04 * np.tanh(-t / w)
        ph = np.cumsum(f0 * dop) / SRA
        engine = sum(a * np.sin(TAU * h * ph) for h, a in ((1, 1.0), (2, 0.5), (3, 0.25), (4, 0.1)))
        s = (0.7 * tyre * prox ** 1.2 + 0.35 * engine * prox) * (0.4 + 0.6 * rng.random())
        d = 1 if rng.random() < 0.5 else -1
        pan = np.clip(d * np.tanh(t / (1.5 * w)) * 0.8, -1, 1)
        a = (pan + 1) * np.pi / 4
        bus._add(bus.L, s * np.cos(a), int((tc - span) * SRA), True)
        bus._add(bus.R, s * np.sin(a), int((tc - span) * SRA), True)
    for ch, buf in ((0, bus.L), (1, bus.R)):
        buf += 0.35 * colored_noise(n, SRA, rng, lo=40, hi=400, order=2, tilt=-2.0)
    _verb(bus, rng, rt=1.2, wet=0.25)
    return _norm([bus.L, bus.R], -24)


def _chirp(rng, kind):
    """One bird call (mono): frequency track x amplitude envelope, a little second harmonic."""
    if kind == "tweet":  # quick falling notes
        notes = []
        k = rng.integers(3, 7)
        base = 3200 + 1500 * rng.random()
        for i in range(k):
            notes.append((i * (0.09 + 0.03 * rng.random()), 0.06, base * (1 - 0.04 * i), -900))
    elif kind == "whistle":  # two clear notes, high then lower
        hi = 3600 + 600 * rng.random()
        notes = [(0.0, 0.28, hi, 120), (0.36, 0.3, hi * 0.83, -80)]
    elif kind == "trill":
        notes = [(0.0, 0.7 + 0.5 * rng.random(), 3000 + 900 * rng.random(), -600)]
    else:  # warble: a wandering phrase
        notes = []
        tt = 0.0
        for i in range(rng.integers(4, 8)):
            d = 0.07 + 0.08 * rng.random()
            notes.append((tt, d, 2400 + 2200 * rng.random(), (rng.random() - 0.5) * 3000))
            tt += d + 0.02 + 0.05 * rng.random()
    total = max(a + d for a, d, _, _ in notes) + 0.05
    m = int(total * SRA)
    out = np.zeros(m)
    for start, d, f, slope in notes:
        i0 = int(start * SRA)
        q = int(d * SRA)
        t = np.arange(q) / SRA
        f_t = f + slope * t / max(d, 1e-3) + 60 * np.sin(TAU * 30 * t)
        if kind == "trill":
            amp = (0.5 + 0.5 * np.sin(TAU * 18 * t)) ** 2
        else:
            amp = np.sin(np.pi * t / d) ** 2
        ph = np.cumsum(f_t) / SRA
        out[i0:i0 + q] += amp * (np.sin(TAU * ph) + 0.12 * np.sin(2 * TAU * ph))
    return out


def birds(seconds, rng, calls_per_min=26):
    n = int(seconds * SRA)
    bus = Bus(n)
    k = int(calls_per_min * seconds / 60)
    # a few "birds", each with its own call, place and loudness, calling in bouts
    flock = [(["tweet", "whistle", "trill", "warble"][i % 4], rng.uniform(-0.8, 0.8), rng.uniform(0.3, 1.0)) for i in range(6)]
    for _ in range(k):
        kind, pan, near = flock[rng.integers(len(flock))]
        s = _chirp(rng, kind)
        if near < 0.55:  # far ones: duller
            s = fft_filter(s, SRA, hi=3800, order=1)
        bus.add(s * near * (0.7 + 0.3 * rng.random()), rng.random() * seconds, SRA, pan + rng.normal(0, 0.1), wrap=True)
    _verb(bus, rng, rt=1.4, wet=0.45)
    return _norm([bus.L, bus.R], -30)


def crickets(seconds, rng):
    """Field crickets: each a tone near 4.5 kHz pulsed in chirps of three or four, at its own rate. Stereo."""
    n = int(seconds * SRA)
    bus = Bus(n)
    for c in range(7):
        f = 4100 + 900 * rng.random()
        rate = 1.4 + 1.6 * rng.random()  # chirps per second
        pulses = int(rng.integers(3, 5))
        gain = rng.uniform(0.25, 1.0)
        pan = rng.uniform(-0.9, 0.9)
        period = 1.0 / rate
        # the chirp, once
        pr = 0.033
        m = int((pulses * pr + 0.02) * SRA)
        t = np.arange(m) / SRA
        pulse_env = np.zeros(m)
        for p in range(pulses):
            tp = t - p * pr
            pulse_env += np.where((tp >= 0) & (tp < 0.018), np.sin(np.pi * np.clip(tp, 0, 0.018) / 0.018) ** 2, 0.0)
        chirp = pulse_env * (np.sin(TAU * f * t) + 0.08 * np.sin(TAU * 2 * f * t))
        if gain < 0.5:
            chirp = fft_filter(chirp, SRA, hi=5200, order=1)
        tt = rng.random() * period
        while tt < seconds:
            # a cricket pauses now and then
            if rng.random() > 0.08:
                bus.add(chirp * gain * (0.85 + 0.15 * rng.random()), tt, SRA, pan, wrap=True)
            tt += period * (0.95 + 0.1 * rng.random())
    _verb(bus, rng, rt=1.1, wet=0.4)
    return _norm([bus.L, bus.R], -30)


def water(seconds, rng):
    """Small waves at a harbour wall: irregular swells of low wash, a fizz of splash after each. Stereo."""
    n = int(seconds * SRA)
    bus = Bus(n)
    tt = 0.0
    while tt < seconds:
        d = 0.8 + 1.4 * rng.random()
        m = int(d * SRA * 1.6)
        t = np.arange(m) / SRA
        env = np.sin(np.pi * np.clip(t / d, 0, 1)) ** 2
        wash = fft_filter(rng.standard_normal(m), SRA, lo=150, hi=900 + 600 * rng.random(), order=2)
        wash /= np.std(wash) + 1e-9
        splash_t = np.clip((t - d * 0.55) / (0.5 * d), 0, None)
        splash = fft_filter(rng.standard_normal(m), SRA, lo=1500, hi=6000, order=2)
        splash /= np.std(splash) + 1e-9
        splash *= np.exp(-splash_t * 5) * (splash_t > 0) * (0.5 + 0.5 * np.abs(np.sin(TAU * 13 * t)))
        s = (wash * env + 0.22 * splash) * (0.5 + 0.5 * rng.random())
        bus.add(s, tt, SRA, rng.uniform(-0.6, 0.6), wrap=True)
        tt += d * (0.55 + 0.4 * rng.random())
    for buf in (bus.L, bus.R):
        buf += 0.25 * colored_noise(n, SRA, rng, lo=80, hi=500, order=2)
    _verb(bus, rng, rt=0.9, wet=0.3)
    return _norm([bus.L, bus.R], -25)


def crowd(seconds, src, src_sr, rng):
    """A denser, longer murmur from the game's own sfx/crowd_murmur clip: copies at random offsets and slight speed changes,
    under Hann windows that overlap by half (constant sum), placed round the loop. Mono."""
    if src_sr != SRA:
        tt = np.arange(int(len(src) * SRA / src_sr)) / SRA
        src = np.interp(tt, np.arange(len(src)) / src_sr, src)
    n = int(seconds * SRA)
    out = np.zeros(n)
    win = int(min(len(src) * 0.9, 4.0 * SRA)) // 2 * 2
    hop = win // 2
    h = np.hanning(win + 1)[:win]
    for layer in range(3):
        pos = int(rng.random() * n)
        for k in range(int(np.ceil(n / hop))):
            speed = 1.0 + rng.uniform(-0.04, 0.04)
            seg_len = int(win * speed) + 2
            st = int(rng.random() * (len(src) - seg_len - 1))
            seg = np.interp(np.arange(win) * speed, np.arange(seg_len), src[st:st + seg_len])
            idx = (pos + k * hop + np.arange(win)) % n
            np.add.at(out, idx, seg * h * (0.8 + 0.4 * rng.random()))
    out = fft_filter(out, SRA, lo=120, hi=5000, order=1, circular=True)
    return _norm([out], -26)


def footstep(rng):
    """A step on pavement: a soft heel thump and a gritty scuff. Mono, 22 kHz."""
    m = int(0.32 * SRA)
    t = np.arange(m) / SRA
    f = 70 + 50 * np.exp(-t / 0.02)
    thump = np.sin(TAU * np.cumsum(f) / SRA) * np.exp(-t / 0.035)
    grit = fft_filter(rng.standard_normal(m), SRA, lo=700, hi=4500 + 1500 * rng.random(), order=2)
    grit /= np.std(grit) + 1e-9
    heel = grit * np.exp(-t / 0.018) * (1 - np.exp(-t / 0.002))
    t2 = np.clip(t - (0.06 + 0.03 * rng.random()), 0, None)
    toe = grit[::-1] * np.exp(-t2 / 0.03) * (t2 > 0) * (1 - np.exp(-t2 / 0.004)) * 0.5
    s = 0.8 * thump + 0.35 * heel + toe * 0.35
    s *= np.clip((0.32 - t) / 0.05, 0, 1)
    s = fft_filter(s, SRA, lo=45, order=2)  # the thump's sweep leaves no offset
    s *= np.clip((0.32 - t) / 0.03, 0, 1)
    return [s / np.max(np.abs(s)) * 0.7]
