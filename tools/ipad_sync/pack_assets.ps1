# Gathers what the iOS city game reads into one folder, laid out like the app's Documents folder (docs/ipad_loader.md):
#   <Out>\assets\...   the game's assets (the Windows assets\ folder minus previews, tools and screenshots)
#   <Out>\shaders\     optional *.metal overrides: empty unless -Shaders (then the built-in library is replaced by these)
# Copy the contents of <Out> into the app's Documents folder (Files app, or Apple Devices > File Sharing).
param(
    [string]$Assets = "$PSScriptRoot\..\..\assets",
    [string]$Out = "$PSScriptRoot\..\..\..\CityEngine_iPad",
    [switch]$Shaders
)
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path "$PSScriptRoot\..\.."
$Assets = (Resolve-Path $Assets).Path
New-Item -ItemType Directory -Force "$Out\assets", "$Out\shaders" | Out-Null
$Out = (Resolve-Path $Out).Path

# Mirror the assets: screenshots, preview pages, generator scripts and backups stay on the PC.
robocopy $Assets "$Out\assets" /MIR /NFL /NDL /NJH /NP /R:1 /W:1 `
    /XD shots industry_variants_backup LodCache __pycache__ `
    /XF *.html *.js *.py *.genlog *.blend *.blend1 | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

# The GUI atlas and the UI tick (the app also carries these; a copy here keeps Documents complete).
New-Item -ItemType Directory -Force "$Out\assets\gui", "$Out\assets\audio\sfx" | Out-Null
Copy-Item "$repo\assets_src\gui\glass_atlas.png", "$repo\assets_src\gui\glass_atlas.txt" "$Out\assets\gui" -Force
Copy-Item "$repo\assets_src\gui\ui_category_select.mp3" "$Out\assets\audio\sfx" -Force

# [title] The logo of the loading / title screens and the example city that becomes Save 1 (the app carries these too).
New-Item -ItemType Directory -Force "$Out\assets\branding", "$Out\assets\saves_template" | Out-Null
New-Item -ItemType Directory -Force "$Outsaves" | Out-Null
Copy-Item "$repo\assets_src\branding\optima_engine_1024.png" "$Out\assets\branding" -Force
Copy-Item "$repo\assets_src\saves_template\bayside.*" "$Out\assets\saves_template" -Force

# The City News lines (the radio's written headlines, the neighbour pairs): on the PC the game reads src\city\, on the
# iPad assets\news\ (city_radio.inl, city_sim.inl). Without it the feed shows plain fallback headlines.
New-Item -ItemType Directory -Force "$Out\assets\news" | Out-Null
Copy-Item "$repo\src\city\news_lines.json" "$Out\assets\news" -Force

# What the radio and the sim read; a missing one is only a warning (the game runs without it, quieter or untuned).
foreach ($need in "audio\radio", "audio\music\playlist.txt", "audio\ambience", "sim\tuning.json", "buildings") {
    if (-not (Test-Path "$Out\assets\$need")) { Write-Warning "assets\$need is missing from $Assets" }
}
# The rail junction planes' textures (switches, diamonds, crossovers, level crossings: <layout>_albedo.jpg + _height.png and
# props_albedo.png; city_rail_junctions.inl reads them, only the layouts a city uses). Without them junctions draw nothing.
$junction = @(Get-ChildItem "$Out\assets\rail\junction\tex" -File -ErrorAction SilentlyContinue)
if ($junction.Count -eq 0) {
    Write-Warning "assets\rail\junction\tex is missing: run  tools\rail_junction_tex\dump_layouts.bat  then  python tools\rail_junction_tex\gen_junction_tex.py"
} else {
    "assets\rail\junction\tex: {0} files, {1:N1} MB" -f $junction.Count, (($junction | Measure-Object Length -Sum).Sum / 1MB)
}
if (-not (Test-Path "$Out\assets\audio\music\playlist.txt")) {
    Write-Warning "no city music: run  python tools\audio_gen\gen_audio.py  (writes assets\audio\music and assets\audio\ambience)"
}
# The sound the iPad will have (assets\audio is mirrored whole: sfx, radio, music, ambience, zones.txt).
# Copy each of these folders into Documents/assets/audio/ on the iPad.
foreach ($dir in "audio\sfx", "audio\radio", "audio\music", "audio\ambience") {
    $files = @(Get-ChildItem "$Out\assets\$dir" -File -ErrorAction SilentlyContinue)
    $mb = ($files | Measure-Object Length -Sum).Sum / 1MB
    "assets\{0}: {1} files, {2:N1} MB" -f $dir, $files.Count, $mb
}

Get-ChildItem "$Out\shaders" -File | Remove-Item
if ($Shaders) { Copy-Item "$repo\src\shaders\msl\*.metal" "$Out\shaders" }

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum)
"{0} files, {1:N0} MB in {2}" -f $size.Count, ($size.Sum / 1MB), $Out
exit 0 # robocopy's 1 ("files copied") is success
