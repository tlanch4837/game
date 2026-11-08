# tools/audio_gen: the game's music and ambience, made from scratch

All of it is synthesised here with numpy: no samples, no downloads, nothing copyrighted. The output goes to the shared
`assets\audio\` folder, which git ignores, so only these scripts are in the repo. Re-running gives the same files (seeded).

```
python tools\audio_gen\gen_audio.py                        # everything, into the main checkout's assets\audio
python tools\audio_gen\gen_audio.py --only music           # or ambience / sfx
python tools\audio_gen\gen_audio.py --only music --track day_1
python tools\audio_gen\gen_audio.py --out D:\somewhere     # elsewhere
```

Needs Python 3 with numpy, and `ffmpeg` / `ffprobe` on PATH (mp3 and flac encoding). Every file prints a check line:
peak and RMS level, DC offset, the share of energy above 8 kHz, and for the loops the jump across the loop point
against the largest ordinary sample step. A line ending in `<-- CHECK` failed a check.

## What it writes

| File | What | Used by |
|---|---|---|
| `music\theme.mp3` | the main theme (hand-written tune, ~72 s), played first when the city opens | city view |
| `music\morning_1.mp3`, `morning_2.mp3` | 5-10 h on the game clock | city view |
| `music\day_1.mp3`, `day_2.mp3` | 10-17 h (very soft shaker and kick) | city view |
| `music\evening_1.mp3` | 17-21 h | city view |
| `music\night_1.mp3`, `night_2.mp3` | 21-5 h | city view |
| `music\playlist.txt` | `name period seconds title` per track: `src/audio.cpp` reads it | city view |
| `ambience\city_air.flac` | open air and soft gusts (stereo, 60 s loop) | city view, always; more when high up |
| `ambience\city_birds.flac` | birds by day (stereo, 60 s loop) | city view, day, fewer over dense city |
| `ambience\city_crickets.flac` | crickets by night (stereo, 40 s loop) | city view, night |
| `ambience\city_hum.flac` | the city far off (mono, 45 s loop) | city view, built-up, from above |
| `ambience\city_traffic.flac` | cars passing a street away (stereo, 60 s loop) | city view, roads in view, low camera |
| `ambience\water.flac` | small waves at a harbour wall (stereo, 40 s loop) | city view, sea near the view |
| `ambience\crowd.flac` | a longer, denser murmur built from `sfx\crowd_murmur.mp3` (mono, 32 s loop) | city view, dense, street level |
| `ambience\wind.flac` | air, for Play mode's default zone bed (`audio.cpp` zones) | Play mode |
| `sfx\footstep_1..4.wav` | a step on pavement | Play mode walking |

Music is 44.1 kHz stereo mp3 at 128 kb/s (the player crossfades tracks, so mp3's start / end padding does not matter).
The beds are 22.05 kHz 16-bit FLAC, because they loop: FLAC is sample-exact, mp3 would leave a gap at the loop point.

## How it is made

- `synth.py`: wavetable and FM oscillators, envelopes, FFT filters (circular for loops), a two-band synthetic reverb,
  stereo buses, dithered 16-bit export, the checks.
- `music.py`: the instruments (soft three-voice pad, FM electric piano, harp, marimba, celesta, sine bass, shaker, soft
  kick), voice-led four-chord progressions, phrase-based melodies (motif, its repeat fitted to the next chord, a cadence on
  a chord tone), arrangements that add and drop layers section by section, ping-pong echo and reverb, a gentle roll-off
  above 9.5 kHz, -19 dBFS RMS with peaks under -1.5 dBFS. The theme's tune is written out by hand in `theme_melody`.
  To add a track: append to `TRACKS` (name `<period>_<n>`), re-run with `--only music`.
- `ambience.py`: each bed is a seamless loop by construction: noise is filtered over the loop length in the frequency
  domain, slow modulations are circular, events (a chirp, a car, a wave) that run past the end wrap to the start, and
  the reverb is a circular convolution.
