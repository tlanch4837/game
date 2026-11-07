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

Get-ChildItem "$Out\shaders" -File | Remove-Item
if ($Shaders) { Copy-Item "$repo\src\shaders\msl\*.metal" "$Out\shaders" }

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum)
"{0} files, {1:N0} MB in {2}" -f $size.Count, ($size.Sum / 1MB), $Out
exit 0 # robocopy's 1 ("files copied") is success
