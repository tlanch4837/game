# Engine screenshots of the Glass Dock+ GUI in the states shots_mockup.py captures (shots/gui_1to1/<dev>_<state>_engine.png).
#   powershell -File tools\gui_atlas\shots_engine.ps1 [state filter]
param([string]$only = '')
$w = Resolve-Path "$PSScriptRoot\..\.."
$states = @(
    @('phone', '2556 1179', 'touch_closed', 'touch'), @('phone', '2556 1179', 'touch_open', 'touch,open'),
    @('phone', '2556 1179', 'touch_roads', 'touch,cat=roads'), @('phone', '2556 1179', 'touch_place', 'touch,pick=Two-Lane Road'),
    @('phone', '2556 1179', 'pause', 'touch,pause'), @('phone', '2556 1179', 'settings', 'touch,settings'),
    @('phone', '2556 1179', 'pc_closed', 'pc'), @('phone', '2556 1179', 'pc_roads', 'pc,cat=roads'),
    @('pad', '2360 1640', 'pc_closed', 'pc'), @('pad', '2360 1640', 'pc_roads', 'pc,cat=roads'), @('pad', '2360 1640', 'pc_services', 'pc,cat=serv'),
    @('pad', '2360 1640', 'pc_place', 'pc,pick=Two-Lane Road'), @('pad', '2360 1640', 'pause', 'pc,pause'), @('pad', '2360 1640', 'settings', 'pc,settings'),
    @('pad', '2360 1640', 'touch_closed', 'touch'), @('pad', '2360 1640', 'touch_roads', 'touch,cat=roads'))
foreach ($s in $states) {
    $name = "$($s[0])_$($s[2])"
    if ($only -and $name -notlike "*$only*") { continue }
    $env:TBD_NOREMOTE = '1'; $env:TBD_GUI = '1'; $env:TBD_VIEWPORT = $s[1]; $env:TBD_GUI_DEVICE = $s[0]; $env:TBD_GUI_STATE = $s[3]; $env:TBD_SETTLE = '30'
    $env:TBD_CAPTURE = "$w\shots\gui_1to1\${name}_engine.png"
    $p = Start-Process -FilePath "$w\bin\tbd_engine.exe" -PassThru -WorkingDirectory "$w\bin"
    if (-not $p.WaitForExit(240000)) { $p.Kill(); "$name killed" } else { "$name done" }
}
