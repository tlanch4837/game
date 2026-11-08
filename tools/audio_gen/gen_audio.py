"""Renders the game's music, ambience beds and footsteps into <out> (default: the shared assets\\audio folder).

    python tools\\audio_gen\\gen_audio.py [--out DIR] [--only music|ambience|sfx] [--track NAME]

Needs numpy and ffmpeg (on PATH) for the mp3 / flac encoding. Writes:
    music\\theme.mp3, music\\<period>_<n>.mp3, music\\playlist.txt   (src/audio.cpp reads the playlist)
    ambience\\city_*.flac, ambience\\water.flac, ambience\\crowd.flac, ambience\\wind.flac   (seamless loops)
    sfx\\footstep_1..4.wav
and prints a level / loop-seam check per file. Seeded: a re-run gives the same audio.
"""
import argparse
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ambience  # noqa: E402
import music  # noqa: E402
from synth import SR, encode, report, write_wav  # noqa: E402

DEFAULT_OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "..", "assets", "audio"))


def default_out():
    # tools\audio_gen -> repo root; the assets folder is the main checkout's (worktrees live under .claude\worktrees)
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.normpath(os.path.join(here, "..", ".."))
    for cand in (os.path.join(repo, "assets", "audio"), DEFAULT_OUT, r"C:\custom windows apps\cityengine\assets\audio"):
        if os.path.isdir(os.path.dirname(cand)):
            return cand
    return os.path.join(repo, "assets", "audio")


def size_kb(p):
    return os.path.getsize(p) / 1024.0


def do_music(out, only_track=None):
    d = os.path.join(out, "music")
    os.makedirs(d, exist_ok=True)
    rows = []
    jobs = [("theme", "theme", "Shortville (Main Theme)", None)] + list(music.TRACKS)
    for i, (name, period, title, spec) in enumerate(jobs):
        mp3 = os.path.join(d, name + ".mp3")
        if only_track and name != only_track:
            if os.path.exists(mp3):
                rows.append((name, period, _duration(mp3), title))
            continue
        t0 = time.time()
        L, R = music.render_theme(1000) if spec is None else music.render(spec, 2000 + i * 37)
        ok = report(name, [L, R], SR)
        wav = os.path.join(d, name + ".wav")
        write_wav(wav, [L, R], SR, np.random.default_rng(i))
        encode(wav, mp3, "mp3")
        secs = len(L) / SR
        rows.append((name, period, secs, title))
        print("   -> %s  %.0f KB  (%.1fs to render)%s" % (mp3, size_kb(mp3), time.time() - t0, "" if ok else "  CHECK LEVELS"))
    with open(os.path.join(d, "playlist.txt"), "w", newline="\n") as f:
        f.write("# The city builder's music (tools/audio_gen/gen_audio.py). One track per line: name period seconds title\n")
        f.write("# period: theme (plays first), morning 5-10h, day 10-17h, evening 17-21h, night 21-5h (the game clock)\n")
        for name, period, secs, title in rows:
            f.write("%s %s %.2f %s\n" % (name, period, secs, title))


def _duration(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path], capture_output=True, text=True)
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 0.0


def _decode_mono(path, sr):
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path, "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"],
                       capture_output=True, check=True)
    return np.frombuffer(r.stdout, dtype=np.float32).astype(np.float64)


def do_ambience(out):
    d = os.path.join(out, "ambience")
    os.makedirs(d, exist_ok=True)
    sra = ambience.SRA
    beds = [
        ("city_air", lambda r: ambience.air(60.0, r)),
        ("wind", lambda r: ambience.air(50.0, r)),  # Play mode's default bed (audio.cpp zones)
        ("city_hum", lambda r: ambience.hum(45.0, r)),
        ("city_traffic", lambda r: ambience.traffic(60.0, r)),
        ("city_birds", lambda r: ambience.birds(60.0, r)),
        ("city_crickets", lambda r: ambience.crickets(40.0, r)),
        ("water", lambda r: ambience.water(40.0, r)),
    ]
    crowd_src = os.path.join(out, "sfx", "crowd_murmur.mp3")
    if os.path.exists(crowd_src):
        src = _decode_mono(crowd_src, sra)
        beds.append(("crowd", lambda r: ambience.crowd(32.0, src, sra, r)))
    else:
        print("   (no sfx\\crowd_murmur.mp3: ambience\\crowd skipped)")
    for i, (name, fn) in enumerate(beds):
        t0 = time.time()
        rng = np.random.default_rng(500 + i * 11)
        chans = fn(rng)
        report(name, chans, sra, loop=True)
        wav = os.path.join(d, name + ".wav")
        flac = os.path.join(d, name + ".flac")
        write_wav(wav, chans, sra, rng)
        encode(wav, flac, "flac")
        print("   -> %s  %.0f KB  (%.1fs)" % (flac, size_kb(flac), time.time() - t0))


def do_sfx(out):
    d = os.path.join(out, "sfx")
    os.makedirs(d, exist_ok=True)
    for k in range(1, 5):
        rng = np.random.default_rng(900 + k)
        chans = ambience.footstep(rng)
        report("footstep_%d" % k, chans, ambience.SRA)
        p = os.path.join(d, "footstep_%d.wav" % k)
        write_wav(p, chans, ambience.SRA, rng)
        print("   -> %s  %.0f KB" % (p, size_kb(p)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=default_out())
    ap.add_argument("--only", choices=["music", "ambience", "sfx"])
    ap.add_argument("--track")
    a = ap.parse_args()
    print("Writing to", a.out)
    if a.only in (None, "music"):
        do_music(a.out, a.track)
    if a.only in (None, "ambience"):
        do_ambience(a.out)
    if a.only in (None, "sfx"):
        do_sfx(a.out)


if __name__ == "__main__":
    main()
