# Loading the game onto the iPad

The Codemagic build carries only the code, the shaders, the GUI atlas and the UI tick. The game's assets live in the app's
**Documents** folder, so new content is tested by copying files, not by a new build. Only C++ changes need Codemagic.

## Layout of Documents

| Folder | What | Read by |
|---|---|---|
| `assets/` | the game's assets (as the Windows `assets\`) | `host::Init` root and `ExeDirectory() + "\\..\\assets"` |
| `bin/` | stands in for the Windows exe folder; empty | `ExeDirectory()` |
| `shaders/` | optional `<name>.metal` overrides; each replaces that function of the built-in library | `renderer_metal.mm` `Fn` |
| `saves/` | game saves | |
| `engine.log` | the log of the last run (timings, draws, errors): copy it back to the PC | `log_apple.cpp` |

The app creates these folders on start and copies its built-in GUI files into `assets/` when missing. Files already
there win. Game paths keep their Windows `\` separators; `tbd::platform::FsPath` turns them into `/` on Apple.

## Making the folder

```
powershell -File tools\ipad_sync\pack_assets.ps1 -Assets "C:\custom windows apps\cityengine\assets"
```

It writes `..\CityEngine_iPad\` (assets without screenshots, preview pages, scripts and backups). `-Shaders` also fills
`shaders\` with `src\shaders\msl\*.metal`, to test a shader change without a build.

## Putting it on the iPad

- **USB from Windows:** Apple Devices app → the iPad → Files → cityengine → drag the *contents* of `CityEngine_iPad`
  (`assets`, `shaders`) in.
- **Files app:** copy the folder to iCloud Drive or a USB stick, then move it into On My iPad → cityengine.

Restart the app after copying. Its first log line says where it read assets from.
