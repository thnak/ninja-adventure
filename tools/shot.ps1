# Dùng: tools\shot.ps1 [-Scene res://content/map/map_village.tscn] [-Out shot.png] [-Frames 90]
param(
    [string]$Scene = "res://main.tscn",
    [string]$Out = "$PSScriptRoot\..\shot.png",
    [int]$Frames = 90,
    [string]$Camera = ""   # "cx,cy,zoom" ví dụ "-300,100,1.2"
)
$godot = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Depth 2 -Filter "Godot*_win64_console.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $godot) { throw "Godot not found; winget install GodotEngine.GodotEngine" }
$game = Resolve-Path "$PSScriptRoot\..\game"
$outAbs = [System.IO.Path]::GetFullPath($Out)
$p = Start-Process -FilePath $godot.FullName -PassThru -NoNewWindow -ArgumentList (@(
    "--rendering-method", "gl_compatibility", "--path", $game, "--script", "$PSScriptRoot\shot.gd", "--", $Scene, $outAbs, $Frames) + $(if ($Camera) { $Camera.Split(",") } else { @() }))
if (-not $p.WaitForExit(60000)) { $p.Kill(); throw "timeout" }
if (Test-Path $outAbs) { Write-Output $outAbs } else { throw "no screenshot produced" }
