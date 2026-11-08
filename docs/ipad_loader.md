# Loading the game onto the iPad

Target: **iPad, Ultra preset** (40 fps; the presets are in src/gfx_settings.h, picked in the pause menu's graphics page). The Codemagic build
carries only the code, the built-in Metal shader library, the GUI atlas and the UI tick. The game's assets live in the
app's **Documents** folder, so new content is tested by copying files, not by a new build. Only C++ changes need Codemagic.

## Layout of Documents

| Folder | What | Read by |
|---|---|---|
| `assets/` | the game's assets (as the Windows `assets\`), plus `assets/news/` and `assets/gui/` from the repo (below) | `host::Init` root and `ExeDirectory() + "\\..\\assets"` |
| `assets/news/news_lines.json` | the City News headlines and the neighbour-pair lines ("adj:school+industry") | `city_radio.inl` LoadNews, `city_sim.inl` SimInit |
| `assets/audio/radio/` | the radio clips (Elton, call-ins, conversations, The Weekly Rundown) | `audio.cpp` (missing clips are silent) |
| `assets/audio/music/` | the city view's music (theme + 7 tracks, mp3) and `playlist.txt` | `audio.cpp` LoadMusic (no playlist: no city music) |
| `assets/audio/ambience/` | the city view's ambience loops (FLAC) and Play mode's `wind.flac` | `audio.cpp` beds (a missing bed is silent) |
| `assets/audio/sfx/` | UI ticks, build / demolish / cash / siren sounds, footsteps | `audio.cpp`, `city_audio.inl` |
| `assets/sim/tuning.json` | the sim's tuning (optional; compiled-in defaults without it) | `city_sim.inl` SimLoadTuning |
| `assets/gui/` | `glass_atlas.png` / `.txt` (Glass Dock+ icons and previews) | `glass_dock_host.inl` LoadAtlas |
| `bin/` | stands in for the Windows exe folder; empty | `ExeDirectory()` |
| `shaders/` | **optional** `<name>.metal` overrides; each replaces that function of the built-in library. Leave it empty normally | `renderer_metal.mm` `Fn` |
| `saves/` | game saves (written by the game; the budget field with loans is new, older saves still load) | |
| `engine.log` | the log of the last run (timings, draws, errors): copy it back to the PC | `log_apple.cpp` |

The app creates these folders on start and copies its built-in GUI files into `assets/` when missing. Files already
there win. Game paths keep their Windows `\` separators; `tbd::platform::FsPath` turns them into `/` on Apple.

`news_lines.json` lives in `src\city\` in the repo (the PC build reads it there). On the iPad there is no `src\`, so it
must be in `assets/news/`; without it the City News card shows plain fallback headlines ("School built.") and the
radio's neighbour stories (a school opened next to industry) never fire. pack_assets.ps1 copies it.

## Making the folder

```
powershell -File tools\ipad_sync\pack_assets.ps1 -Assets "C:\custom windows apps\cityengine\assets"
```

It writes `..\CityEngine_iPad\` with:

- `assets\` mirrored from `-Assets`, without screenshots (`shots`), preview pages (`*.html`, `*.js`), generator scripts
  (`*.py`, `*.genlog`), Blender files, `LodCache` and backups;
- `assets\gui\glass_atlas.png`, `glass_atlas.txt` and `assets\audio\sfx\ui_category_select.mp3` from `assets_src\`;
- `assets\news\news_lines.json` from `src\city\`;
- `shaders\` empty (with `-Shaders`: `src\shaders\msl\*.metal`, to test a shader change without a build).

It warns when `assets\audio\radio`, `assets\audio\music\playlist.txt`, `assets\audio\ambience`, `assets\sim\tuning.json`
or `assets\buildings` is missing from the source, and prints the file count and size of `assets\audio\sfx`, `radio`,
`music` and `ambience`.

The app itself carries the GUI atlas, the GUI's SDF font (`assets_src\gui\ui_font_sdf.png`) and the UI tick, and copies them
into Documents on first run when missing, so the GUI and its tick work before anything is loaded.

Sound needs `Documents/assets/audio/` with these four folders (sizes as generated on 2026-11-08):

| Copy to `Documents/assets/audio/` | Files | Size | What |
|---|---|---|---|
| `sfx\` | 18 | 0.4 MB | UI ticks, build_place, demolish, cash_register, siren_pass, crowd_murmur, footstep_1..4 |
| `radio\` | 521 | 96 MB | the radio clips |
| `music\` | 9 | 17.6 MB | `theme.mp3`, `morning_1/2`, `day_1/2`, `evening_1`, `night_1/2` (.mp3), `playlist.txt` |
| `ambience\` | 8 | 14.7 MB | `city_air`, `city_birds`, `city_crickets`, `city_hum`, `city_traffic`, `water`, `crowd`, `wind` (.flac) |

`music\` and `ambience\` (and the footsteps in `sfx\`) are made by `python tools\audio_gen\gen_audio.py` (see
tools/audio_gen/README.md) into the PC's `assets\audio\`; pack_assets.ps1 copies them like everything else. After
adding only the music, copying just `assets\audio\music` and `assets\audio\ambience` (about 32 MB) into
`Documents/assets/audio/` is enough. A pack made before the radio clips existed has no `audio\radio`: re-run
pack_assets.ps1 and copy `assets\audio\radio` (or the whole `assets\audio`) into `Documents/assets/audio/`.

What plays in the city view: the theme when a city opens, then tracks for the game clock's time of day, crossfaded,
sometimes with a short pause; the music dips while Elton talks; ambience follows the camera (air up high, birds or
crickets, traffic and people at street level, water near the coast). Volumes: pause menu > Settings > Sound and music
(Master, Music, Ambience, Effects, Radio; saved in `saves/settings.ini` as `vol_*` in percent). `engine.log` says
`Audio: 8 music tracks in music\playlist.txt` when the music was found (or that there is no playlist).

## What to copy after a code-only change

A Codemagic build with only C++ changes needs no copy, except when it adds data files. The sim pass ported onto
ios-metal (construction charges, info panels, the radio's neighbour stories) needs only `assets/news/news_lines.json`
beyond what was already there; re-run pack_assets.ps1 or copy that one file into `Documents/assets/news/`.

## Putting it on the iPad

- **USB from Windows:** Apple Devices app → the iPad → Files → cityengine → drag the *contents* of `CityEngine_iPad`
  (`assets`, and `shaders` only if you used `-Shaders`) in.
- **Files app:** copy the folder to iCloud Drive or a USB stick, then move it into On My iPad → cityengine.

Restart the app after copying. Its first log line says where it read assets from; `Radio: N written headlines` and
`Sim: N news lines` in `engine.log` confirm the news file was found.
