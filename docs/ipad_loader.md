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

It warns when `assets\audio\radio`, `assets\sim\tuning.json` or `assets\buildings` is missing from the source.

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
