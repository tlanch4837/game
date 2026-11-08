"""The city builder's background music: calm ambient tracks by time of day, plus a short main theme. All original, made
from scratch by these rules (seeded, so a re-run gives the same files).

A track: a key, a tempo, a four-chord progression, and an arrangement of sections that add and drop layers (pad, bass,
plucked arpeggio, a melody on an electric piano / harp / marimba / celesta, and in the day tracks a very soft shaker).
Melodies are made of 4-bar phrases: a motif, its repeat fitted to the next chord, and a cadence on a chord tone.
"""
import numpy as np

from synth import SR, TAU, Bus, ar_env, fft_filter, hz, lin_convolve, make_ir, tab_osc, table

# ------------------------------------------------------------------------------------------------ instruments
# Each returns (left, right) or a mono array, already enveloped, starting at the note's onset.


def pad_note(m, dur, vel, rng, bright=0.45, attack=1.8, release=3.2, kind="warm"):
    f = hz(m)
    n = int((dur + release) * SR)
    t = np.arange(n) / SR
    nh = int(max(1, min(14, 4000.0 / f)))  # a soft analogue-style pad: nothing above 4 kHz
    dark, lit = table("dark", nh), table(kind, nh)
    L = np.zeros(n)
    R = np.zeros(n)
    for i, cents in enumerate((-7.0, 0.0, 6.0)):
        fi = f * 2.0 ** (cents / 1200.0)
        ph = rng.random() + fi * t
        w = bright * (0.5 + 0.5 * np.sin(TAU * (0.05 + 0.02 * i) * t + rng.random() * TAU))
        v = (1.0 - w) * tab_osc(dark, ph) + w * tab_osc(lit, ph)
        a = (np.array([-0.55, 0.0, 0.55])[i] + 1.0) * np.pi / 4.0
        L += v * np.cos(a)
        R += v * np.sin(a)
    env = ar_env(n, SR, attack, dur, release) * (1.0 + 0.05 * np.sin(TAU * 0.21 * t + rng.random() * TAU))
    g = vel * env / 4.5  # three voices, and a chord is four notes: the pad sits under the melody
    return L * g, R * g


def ep_note(m, dur, vel, rng):
    """Electric piano: 1:1 FM with a decaying index, a soft tine on top."""
    f = hz(m)
    decay = 2.4 * 2.0 ** (-(m - 60) / 24.0)
    rel = 0.35
    n = int((min(dur, 6.0) + rel + 0.05) * SR)
    t = np.arange(n) / SR
    index = (1.0 * vel + 0.25) * np.exp(-t / 0.45) + 0.12
    car = np.sin(TAU * f * t + index * np.sin(TAU * f * t))
    tine = 0.05 * vel * np.sin(TAU * f * 4.0 * t) * np.exp(-t / 0.06) if f * 4 < 7000 else 0.0
    env = (1.0 - np.exp(-t / 0.003)) * np.exp(-t / decay) * ar_env(n, SR, 0.001, min(dur, 6.0), rel)
    return (car + tine) * env * vel


def harp_note(m, dur, vel, rng, length=2.6):
    """Plucked string: partials shaped by the pluck point, upper ones dying faster."""
    f = hz(m)
    n = int(min(length * 1.6, dur + length) * SR)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for h in range(1, 11):
        fh = f * h * (1.0 + 0.0003 * h * h)
        if fh > 8000:
            break
        a = abs(np.sin(np.pi * h * 0.19)) / h
        tau = length / (1.0 + 0.8 * (h - 1))
        out += a * np.sin(TAU * fh * t + rng.random() * TAU) * np.exp(-t / tau)
    env = 1.0 - np.exp(-t / 0.0025)
    return out * env * vel * 0.8


def marimba_note(m, dur, vel, rng):
    f = hz(m)
    n = int(1.6 * SR)
    t = np.arange(n) / SR
    k = 2.0 ** (-(m - 60) / 30.0)
    out = np.sin(TAU * f * t) * np.exp(-t / (0.65 * k))
    if f * 3.93 < 8000:
        out += 0.22 * np.sin(TAU * f * 3.93 * t) * np.exp(-t / (0.12 * k))
    if f * 9.8 < 8000:
        out += 0.05 * np.sin(TAU * f * 9.8 * t) * np.exp(-t / (0.03 * k))
    return out * (1.0 - np.exp(-t / 0.0015)) * vel


def celesta_note(m, dur, vel, rng):
    f = hz(m)
    n = int(3.2 * SR)
    t = np.arange(n) / SR
    out = np.zeros(n)
    for r, a, tau in ((1.0, 1.0, 2.2), (2.0, 0.35, 1.2), (3.0, 0.12, 0.6), (4.16, 0.06, 0.35), (5.43, 0.03, 0.2)):
        if f * r < 9000:
            out += a * np.sin(TAU * f * r * t + rng.random() * TAU) * np.exp(-t / tau)
    return out * (1.0 - np.exp(-t / 0.002)) * vel * 0.7


def bass_note(m, dur, vel, rng):
    f = hz(m)
    rel = 0.35
    n = int((dur + rel) * SR)
    t = np.arange(n) / SR
    out = np.sin(TAU * f * t) + 0.22 * np.sin(TAU * 2 * f * t) + 0.06 * np.sin(TAU * 3 * f * t)
    env = ar_env(n, SR, 0.03, dur, rel) * (0.78 + 0.22 * np.exp(-t / 0.9))
    return out * env * vel


def shaker_hit(vel, rng):
    n = int(0.09 * SR)
    t = np.arange(n) / SR
    w = rng.standard_normal(n)
    w = np.diff(np.diff(w, prepend=0.0), prepend=0.0)  # tilted up: a soft hiss, not a click
    w = fft_filter(w, SR, lo=3500, hi=9000, order=2)
    env = (1.0 - np.exp(-t / 0.006)) * np.exp(-t / 0.028)
    return w / (np.max(np.abs(w)) + 1e-9) * env * vel


def kick_hit(vel, rng):
    n = int(0.45 * SR)
    t = np.arange(n) / SR
    f = 46.0 + 38.0 * np.exp(-t / 0.035)
    ph = np.cumsum(f) / SR
    return np.sin(TAU * ph) * np.exp(-t / 0.2) * (1.0 - np.exp(-t / 0.004)) * vel


LEAD = {"ep": ep_note, "harp": harp_note, "marimba": marimba_note, "celesta": celesta_note}

# ------------------------------------------------------------------------------------------------ harmony
QUAL = {
    "maj7": [0, 4, 7, 11], "m7": [0, 3, 7, 10], "maj9": [0, 4, 7, 11, 14], "m9": [0, 3, 7, 10, 14],
    "add9": [0, 4, 7, 14], "sus2": [0, 2, 7], "6": [0, 4, 7, 9], "69": [0, 4, 7, 9, 14], "7sus4": [0, 5, 7, 10],
    "maj7s11": [0, 4, 7, 11, 18], "m6": [0, 3, 7, 9], "maj": [0, 4, 7], "m": [0, 3, 7],
}
SCALES = {"major": [0, 2, 4, 5, 7, 9, 11], "dorian": [0, 2, 3, 5, 7, 9, 10], "minor": [0, 2, 3, 5, 7, 8, 10],
          "lydian": [0, 2, 4, 6, 7, 9, 11]}


def chord_pcs(key, chord):
    off, q = chord
    return [(key + off + i) % 12 for i in QUAL[q]]


def voice_chord(pcs, prev, lo=52, hi=74):
    """Up to four pad notes, each the octave of its pitch class closest to the previous voicing (smooth voice leading)."""
    use = list(pcs)
    if len(use) > 4:
        use = use[1:]  # the bass has the root
    if len(use) > 4:
        use = [p for i, p in enumerate(use) if i != 1]  # drop the fifth of a big chord
    centre = np.mean(prev) if prev else (lo + hi) / 2.0
    notes = []
    for pc in use:
        cands = [m for m in range(lo, hi + 1) if m % 12 == pc]
        ref = prev if prev else [centre]
        best = min(cands, key=lambda m: min(abs(m - r) for r in ref) + 0.15 * abs(m - centre))
        while best in notes:
            best += 12
        notes.append(best)
    notes.sort()
    # no semitone rubs in the low half
    for i in range(1, len(notes)):
        if notes[i] - notes[i - 1] == 1 and notes[i] < 64:
            notes[i - 1] -= 12 if notes[i - 1] - 12 >= lo - 5 else 0
    return sorted(notes)


def bass_root(key, chord, lo=36, hi=47):
    pc = (key + chord[0]) % 12
    return [m for m in range(lo, hi + 1) if m % 12 == pc][0]


# ------------------------------------------------------------------------------------------------ melody
RHYTHMS = [[1.5, 0.5, 2.0], [1.0, 1.0, 2.0], [0.5, 0.5, 1.0, 2.0], [2.0, 1.0, 1.0], [1.0, 0.5, 0.5, 2.0], [3.0, 1.0],
           [1.5, 1.5, 1.0], [0.5, 1.0, 0.5, 2.0]]


def scale_notes(key, scale, lo, hi):
    pcs = {(key + s) % 12 for s in SCALES[scale]}
    return [m for m in range(lo, hi + 1) if m % 12 in pcs]


def phrase(rng, key, scale, chords, lo, hi, start_pitch, sparse=False):
    """One 4-bar phrase over four chords (one per bar): [(beat, beats, midi, vel)]."""
    notes = scale_notes(key, scale, lo, hi)
    idx = int(np.argmin([abs(n - start_pitch) for n in notes]))
    a = RHYTHMS[rng.integers(len(RHYTHMS))]
    b = RHYTHMS[rng.integers(len(RHYTHMS))]
    plan = [a, b, a, [4.0] if not sparse else [2.0, 2.0]]
    out = []
    motif = None
    for bar, rhythm in enumerate(plan):
        pcs = chord_pcs(key, chords[bar])
        beat = 0.0
        steps = []
        for k, d in enumerate(rhythm):
            strong = beat in (0.0, 2.0)
            if bar == 2 and motif is not None and k < len(motif):  # the motif again, moved to fit this chord
                idx = int(np.clip(idx + motif[k], 0, len(notes) - 1))
            else:
                r = rng.random()
                step = rng.choice([-1, 1]) if r < 0.62 else (rng.choice([-2, 2]) if r < 0.85 else 0)
                # lean back toward the middle of the range
                if notes[idx] > (lo + hi) / 2 + 5:
                    step = -abs(step)
                elif notes[idx] < (lo + hi) / 2 - 5:
                    step = abs(step)
                idx = int(np.clip(idx + step, 0, len(notes) - 1))
            if strong or bar == 3:  # strong beats land on chord tones
                ct = [i for i, n in enumerate(notes) if n % 12 in pcs]
                idx = min(ct, key=lambda i: abs(i - idx))
            steps.append(idx)
            vel = 0.62 + 0.12 * (1.0 if strong else 0.0) + 0.06 * rng.standard_normal()
            if not (sparse and bar == 3 and k == 1):
                out.append((bar * 4 + beat, d, notes[idx], float(np.clip(vel, 0.35, 0.85))))
            beat += d
        if bar == 0:
            motif = [0] + [steps[i] - steps[i - 1] for i in range(1, len(steps))]
            motif[0] = 0
    return out, notes[idx]


# ------------------------------------------------------------------------------------------------ the track
def render(spec, seed):
    """spec: key, scale, bpm, prog [(offset, quality)] one per bar, sections [(bars, {layers})], lead, mood params."""
    rng = np.random.default_rng(seed)
    beat = 60.0 / spec["bpm"]
    bar = 4 * beat
    total_bars = sum(b for b, _ in spec["sections"])
    tail = 6.0
    n = int((total_bars * bar + tail) * SR)
    dry, verb, delay = Bus(n), Bus(n), Bus(n)
    key, prog = spec["key"], spec["prog"]
    lead = LEAD[spec["lead"]]
    arp_inst = LEAD[spec.get("arp", "harp")]
    prev = None
    bar_i = 0
    pitch = spec.get("mel_lo", 67) + 7
    hum = lambda: rng.normal(0.0, 0.006)  # timing, seconds
    for sec_bars, layers in spec["sections"]:
        mel_queue = []
        for b in range(sec_bars):
            t0 = bar_i * bar
            ch = prog[bar_i % len(prog)]
            pcs = chord_pcs(key, ch)
            last = bar_i == total_bars - 1
            if last:
                ch = prog[0]
                pcs = chord_pcs(key, ch)
            # pad: a chord per bar, tied over when the chord repeats
            if "pad" in layers:
                v = voice_chord(pcs, prev, spec.get("pad_lo", 52), spec.get("pad_hi", 74))
                prev = v
                d = bar + (bar * 1.5 if last else 0.15)
                pv = layers["pad"]
                for m in v:
                    L, R = pad_note(m, d, pv, rng, bright=spec.get("bright", 0.45), attack=spec.get("pad_attack", 1.6),
                                    release=spec.get("pad_release", 3.0), kind=spec.get("pad_kind", "warm"))
                    dry.add(L, t0, SR, 0.0, 1.0, right=R)
                    verb.add(L, t0, SR, 0.0, 0.55, right=R)
            if "bass" in layers:
                r = bass_root(key, ch)
                bv = layers["bass"]
                if spec.get("bass_move") and not last and "full" in layers:
                    fifth = r + 7 if r + 7 <= 50 else r - 5
                    dry.add(bass_note(r, 2 * beat - 0.05, bv, rng), t0 + hum(), SR, 0.0)
                    dry.add(bass_note(fifth, 2 * beat - 0.05, bv * 0.85, rng), t0 + 2 * beat + hum(), SR, 0.0)
                else:
                    dry.add(bass_note(r, bar * (2.0 if last else 1.0) - 0.05, bv, rng), t0 + hum(), SR, 0.0)
            if "arp" in layers and not last:
                av = layers["arp"]
                tones = sorted({m for m in range(spec.get("arp_lo", 60), spec.get("arp_hi", 81)) if m % 12 in pcs})
                pat = spec.get("arp_pat", "up")
                step = spec.get("arp_step", 0.5)
                count = int(4 / step)
                if pat == "up":
                    seq = [tones[i % len(tones)] for i in range(count)]
                elif pat == "updown":
                    cyc = tones + tones[-2:0:-1]
                    seq = [cyc[i % len(cyc)] for i in range(count)]
                else:  # broken: low-high pairs
                    seq = [tones[(i // 2 + (i % 2) * 2) % len(tones)] for i in range(count)]
                for i, m in enumerate(seq):
                    if spec.get("arp_skip", 0.0) and rng.random() < spec["arp_skip"]:
                        continue
                    acc = 1.0 if i % 4 == 0 else 0.78
                    s = arp_inst(m, step * beat, av * acc * (0.9 + 0.2 * rng.random()), rng)
                    pan = np.sin(i * 0.9) * 0.45
                    delay.add(s, t0 + i * step * beat + hum(), SR, pan)
                    verb.add(s, t0 + i * step * beat, SR, pan, 0.5)
            if "mel" in layers and b % 4 == 0 and b + 4 <= sec_bars and not (layers.get("mel_alt") and (b // 4) % 2 == 1):
                chs = [prog[(bar_i + k) % len(prog)] for k in range(4)]
                ph, pitch = phrase(rng, key, spec["scale"], chs, spec.get("mel_lo", 67), spec.get("mel_hi", 86), pitch,
                                   sparse=spec.get("sparse", False))
                mel_queue.append((t0, ph))
            if "perc" in layers and not last:
                pv = layers["perc"]
                for i in range(8):
                    sw = 0.035 * beat if i % 2 else 0.0  # a little swing
                    acc = (0.6, 0.35, 0.5, 0.35, 0.6, 0.35, 0.5, 0.4)[i]
                    dry.add(shaker_hit(pv * acc, rng), t0 + i * 0.5 * beat + sw + hum(), SR, 0.25)
                    verb.add(shaker_hit(pv * acc, rng), t0 + i * 0.5 * beat + sw, SR, 0.25, 0.3)
                if layers.get("kick"):
                    for i in (0, 2.5) if b % 2 else (0,):
                        dry.add(kick_hit(layers["kick"], rng), t0 + i * beat, SR, 0.0)
            bar_i += 1
        mv = layers.get("mel", 0.0)
        for t0, ph in mel_queue:
            for (bt, d, m, v) in ph:
                s = lead(m + spec.get("mel_oct", 0), d * beat, v * mv, rng)
                pan = spec.get("mel_pan", -0.12)
                delay.add(s, t0 + bt * beat + hum(), SR, pan, 0.85)
                verb.add(s, t0 + bt * beat, SR, pan, 0.6)
                if spec.get("double") and "double" in layers:  # celesta an octave up, softly
                    s2 = celesta_note(m + 12, d * beat, v * mv * layers["double"], rng)
                    delay.add(s2, t0 + bt * beat + 0.012, SR, 0.3, 0.6)
                    verb.add(s2, t0 + bt * beat, SR, 0.3, 0.6)

    # ping-pong echo on the plucked / melodic bus (dotted eighth), then into the dry mix
    d = int(0.75 * beat * SR)
    echoL, echoR = delay.L.copy(), delay.R.copy()
    mono = 0.5 * (delay.L + delay.R)
    mono_lp = fft_filter(mono, SR, hi=3500, order=1)
    for k in range(1, 5):
        g = 0.32 ** k
        tgt = echoR if k % 2 else echoL
        tgt[k * d:] += g * mono_lp[:-k * d] if k * d < n else 0.0
    dry.L += echoL
    dry.R += echoR

    ir = make_ir(SR, rng, rt_low=spec.get("rt", 3.4), rt_high=1.1, length=spec.get("rt", 3.4) * 1.25)
    wetL = lin_convolve(verb.L, ir[0])[:n]
    wetR = lin_convolve(verb.R, ir[1])[:n]
    wet = spec.get("wet", 0.42)
    L = dry.L + wet * wetL
    R = dry.R + wet * wetR
    # master: no rumble, no fizz (a gentle slope above 9 kHz)
    L = fft_filter(L, SR, lo=32, hi=9500, order=1)
    R = fft_filter(R, SR, lo=32, hi=9500, order=1)
    # fades: in over 1.5 s, out over the last 5 s (the player crossfades tracks anyway)
    t = np.arange(n) / SR
    fade = np.clip(t / 1.5, 0, 1) * np.clip((n / SR - t) / 5.0, 0, 1) ** 1.5
    L *= fade
    R *= fade
    # level: about -19 dBFS RMS, never above -1.5 dBFS peak
    rms = np.sqrt(np.mean(np.concatenate([L, R]) ** 2))
    peak = max(np.max(np.abs(L)), np.max(np.abs(R)))
    g = min(10 ** (-19 / 20) / rms, 10 ** (-1.5 / 20) / peak)
    return L * g, R * g


def theme_melody():
    """The main theme's tune (C major), written by hand: (bar, beat, beats, midi)."""
    a = [
        [(0, 1, 76), (1, .5, 79), (1.5, .5, 81), (2, 1.5, 79), (3.5, .5, 76)],
        [(0, 1, 74), (1, .5, 76), (1.5, .5, 74), (2, 2, 71)],
        [(0, 1, 72), (1, .5, 76), (1.5, .5, 81), (2, 1.5, 79), (3.5, .5, 76)],
        [(0, 1, 77), (1, 1, 76), (2, 2, 72)],
        [(0, 1, 74), (1, .5, 77), (1.5, .5, 81), (2, 1.5, 84), (3.5, .5, 81)],
        [(0, 1, 79), (1, .5, 81), (1.5, .5, 79), (2, 2, 76)],
        [(0, 1, 77), (1, 1, 74), (2, 1, 79), (3, 1, 77)],
        [(0, 4, 76)],
    ]
    return a


def render_theme(seed):
    """The main theme: the hand-written tune over pad, bass and harp; second time round fuller, celesta doubling."""
    rng = np.random.default_rng(seed)
    bpm = 80.0
    beat = 60.0 / bpm
    bar = 4 * beat
    key = 0
    prog = [(0, "add9"), (7, "sus2"), (9, "m7"), (5, "maj7"), (2, "m7"), (5, "maj9"), (7, "7sus4"), (0, "maj")]
    plan = [("intro", 4), ("a", 8), ("b", 8), ("outro", 4)]
    total = sum(b for _, b in plan)
    n = int((total * bar + 7.0) * SR)
    dry, verb, delay = Bus(n), Bus(n), Bus(n)
    mel = theme_melody()
    prev = None
    bi = 0
    for name, bars in plan:
        for b in range(bars):
            t0 = bi * bar
            ch = prog[b % 8] if name in ("a", "b") else prog[b % 4] if name == "intro" else prog[(4 + b) % 8]
            last = bi == total - 1
            if last:
                ch = (0, "add9")
            pcs = chord_pcs(key, ch)
            v = voice_chord(pcs, prev, 52, 72)
            prev = v
            pv = {"intro": 0.5, "a": 0.42, "b": 0.5, "outro": 0.45}[name]
            for m in v:
                L, R = pad_note(m, bar + (bar * 1.5 if last else 0.15), pv, rng, bright=0.55, attack=1.4, release=3.2)
                dry.add(L, t0, SR, 0.0, 1.0, right=R)
                verb.add(L, t0, SR, 0.0, 0.5, right=R)
            if name != "intro" or b >= 2:
                r = bass_root(key, ch)
                dry.add(bass_note(r, bar * (2 if last else 1) - 0.05, 0.42 if name == "b" else 0.34, rng), t0, SR)
            if name in ("b", "outro") and not last:
                tones = sorted({m for m in range(60, 84) if m % 12 in pcs})
                for i in range(8):
                    m = tones[(i * 2 + (i // 4)) % len(tones)]
                    s = harp_note(m, 0.5 * beat, 0.28 if i % 4 else 0.36, rng)
                    delay.add(s, t0 + i * 0.5 * beat, SR, np.sin(i) * 0.4)
                    verb.add(s, t0 + i * 0.5 * beat, SR, 0.0, 0.5)
            if name in ("a", "b"):
                for (bt, d, m) in mel[b]:
                    vel = 0.72 if bt in (0, 2) else 0.6
                    s = ep_note(m - 12 if name == "a" else m - 12, d * beat, vel * 0.75, rng)
                    delay.add(s, t0 + bt * beat, SR, -0.1, 0.9)
                    verb.add(s, t0 + bt * beat, SR, -0.1, 0.6)
                    if name == "b":
                        s2 = celesta_note(m, d * beat, vel * 0.32, rng)
                        delay.add(s2, t0 + bt * beat + 0.01, SR, 0.3, 0.8)
                        verb.add(s2, t0 + bt * beat, SR, 0.3, 0.7)
            bi += 1
    d = int(0.75 * beat * SR)
    mono = fft_filter(0.5 * (delay.L + delay.R), SR, hi=3500, order=1)
    for k in range(1, 4):
        tgt = delay.R if k % 2 else delay.L
        tgt[k * d:] += 0.3 ** k * mono[:-k * d]
    dry.mix(delay)
    ir = make_ir(SR, rng, rt_low=3.6, rt_high=1.2, length=4.5)
    L = dry.L + 0.42 * lin_convolve(verb.L, ir[0])[:n]
    R = dry.R + 0.42 * lin_convolve(verb.R, ir[1])[:n]
    L = fft_filter(L, SR, lo=32, hi=9500, order=1)
    R = fft_filter(R, SR, lo=32, hi=9500, order=1)
    t = np.arange(n) / SR
    fade = np.clip(t / 0.8, 0, 1) * np.clip((n / SR - t) / 5.0, 0, 1) ** 1.5
    L *= fade
    R *= fade
    rms = np.sqrt(np.mean(np.concatenate([L, R]) ** 2))
    peak = max(np.max(np.abs(L)), np.max(np.abs(R)))
    g = min(10 ** (-18 / 20) / rms, 10 ** (-1.5 / 20) / peak)
    return L * g, R * g


# The playlist. period: morning 5-10, day 10-17, evening 17-21, night 21-5 (game clock).
P, B, A, M, PERC = "pad", "bass", "arp", "mel", "perc"
TRACKS = [
    ("morning_1", "morning", "Harbour Morning", dict(
        key=2, scale="major", bpm=72, lead="harp", arp="harp", arp_pat="updown", arp_step=0.5, arp_skip=0.12,
        prog=[(0, "maj9"), (9, "m7"), (5, "maj7"), (7, "69")], bright=0.4, rt=3.6, wet=0.45, mel_lo=69, mel_hi=86,
        sections=[(4, {P: 0.55}), (8, {P: 0.45, B: 0.3, A: 0.22}), (8, {P: 0.4, B: 0.32, A: 0.2, M: 0.55}),
                  (8, {P: 0.5, A: 0.18}), (8, {P: 0.4, B: 0.32, A: 0.22, M: 0.6, "full": 1}), (8, {P: 0.48, B: 0.25, A: 0.14}),
                  (2, {P: 0.5, B: 0.25})])),
    ("morning_2", "morning", "First Light", dict(
        key=7, scale="lydian", bpm=68, lead="celesta", arp="harp", arp_pat="broken", arp_step=1.0,
        prog=[(0, "maj7"), (2, "6"), (9, "m7"), (5, "maj9")], bright=0.35, rt=4.0, wet=0.5, mel_lo=72, mel_hi=88, sparse=True,
        pad_kind="hollow", sections=[(4, {P: 0.55}), (8, {P: 0.48, B: 0.28, A: 0.25}), (8, {P: 0.45, B: 0.3, A: 0.22, M: 0.5}),
                                     (8, {P: 0.52, A: 0.25}), (8, {P: 0.45, B: 0.3, A: 0.22, M: 0.55}), (6, {P: 0.5, B: 0.22})])),
    ("day_1", "day", "Main Street", dict(
        key=5, scale="major", bpm=86, lead="ep", arp="harp", arp_pat="up", arp_step=0.5, arp_skip=0.2, bass_move=True,
        prog=[(0, "maj7"), (4, "m7"), (5, "maj7"), (7, "6")], bright=0.5, rt=2.8, wet=0.36, mel_lo=65, mel_hi=81,
        sections=[(4, {P: 0.5, B: 0.3}), (8, {P: 0.42, B: 0.34, A: 0.2, PERC: 0.07}),
                  (8, {P: 0.38, B: 0.36, A: 0.2, M: 0.6, PERC: 0.08, "kick": 0.22, "full": 1}),
                  (8, {P: 0.48, A: 0.2, M: 0.5, "mel_alt": 1}),
                  (8, {P: 0.38, B: 0.36, A: 0.22, M: 0.62, PERC: 0.08, "kick": 0.22, "full": 1}),
                  (8, {P: 0.42, B: 0.3, A: 0.18, PERC: 0.06}), (4, {P: 0.48, B: 0.28}), (2, {P: 0.5, B: 0.25})])),
    ("day_2", "day", "Zoning Board", dict(
        key=7, scale="major", bpm=92, lead="marimba", arp="marimba", arp_pat="broken", arp_step=0.5, arp_skip=0.25,
        bass_move=True, prog=[(0, "maj7"), (9, "m9"), (5, "maj7s11"), (7, "7sus4")], bright=0.45, rt=2.6, wet=0.34,
        mel_lo=67, mel_hi=84, mel_oct=0, sections=[(4, {P: 0.5}), (8, {P: 0.42, B: 0.32, A: 0.22, PERC: 0.06}),
                                                   (8, {P: 0.38, B: 0.34, A: 0.18, M: 0.62, PERC: 0.07, "full": 1}),
                                                   (8, {P: 0.48, B: 0.25, M: 0.45, "mel_alt": 1}),
                                                   (8, {P: 0.38, B: 0.34, A: 0.2, M: 0.62, PERC: 0.07, "kick": 0.18, "full": 1}),
                                                   (8, {P: 0.42, B: 0.3, A: 0.2, PERC: 0.05}), (8, {P: 0.45, B: 0.28, A: 0.14}),
                                                   (2, {P: 0.5, B: 0.25})])),
    ("evening_1", "evening", "Golden Hour", dict(
        key=3, scale="major", bpm=66, lead="ep", arp="harp", arp_pat="updown", arp_step=1.0, double=True,
        prog=[(0, "maj7"), (9, "m9"), (5, "maj9"), (7, "7sus4")], bright=0.55, rt=3.8, wet=0.46, mel_lo=63, mel_hi=79,
        pad_attack=2.2, sections=[(4, {P: 0.55}), (8, {P: 0.46, B: 0.3, A: 0.24}), (8, {P: 0.42, B: 0.32, A: 0.2, M: 0.6}),
                                  (8, {P: 0.5, A: 0.22, M: 0.45, "mel_alt": 1}),
                                  (8, {P: 0.42, B: 0.32, A: 0.22, M: 0.6, "double": 0.5}), (4, {P: 0.5, B: 0.25}), (2, {P: 0.5})])),
    ("night_1", "night", "Streetlights", dict(
        key=9, scale="minor", bpm=60, lead="celesta", arp="harp", arp_pat="broken", arp_step=1.0, arp_skip=0.3,
        prog=[(0, "m9"), (8, "maj7"), (5, "m9"), (7, "m7")], bright=0.3, rt=4.4, wet=0.52, mel_lo=69, mel_hi=86, sparse=True,
        pad_kind="hollow", pad_attack=2.6, pad_release=3.8,
        sections=[(4, {P: 0.55}), (8, {P: 0.5, B: 0.26, A: 0.2}), (8, {P: 0.45, B: 0.28, A: 0.18, M: 0.5}),
                  (8, {P: 0.55, B: 0.2}), (8, {P: 0.45, B: 0.28, A: 0.2, M: 0.52}), (2, {P: 0.5, B: 0.2})])),
    ("night_2", "night", "Quiet Blocks", dict(
        key=4, scale="dorian", bpm=56, lead="ep", arp="celesta", arp_pat="up", arp_step=1.0, arp_skip=0.35,
        prog=[(0, "m9"), (5, "69"), (8, "maj7"), (10, "add9")], bright=0.3, rt=4.6, wet=0.52, mel_lo=64, mel_hi=79, sparse=True,
        pad_attack=2.8, pad_release=4.0,
        sections=[(4, {P: 0.55}), (8, {P: 0.48, B: 0.26, A: 0.18}), (8, {P: 0.44, B: 0.28, A: 0.16, M: 0.55}),
                  (4, {P: 0.55, B: 0.2}), (8, {P: 0.44, B: 0.28, A: 0.18, M: 0.55}), (2, {P: 0.5, B: 0.2})])),
]
