# Chạy các bài kiểm tra có cửa sổ (renderer Compatibility). Dùng: tools\test_interact.ps1 [travel|interact]
param([string]$Test = "interact", [string]$OutDir = "$env:TEMP")
$godot = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Depth 2 -Filter "Godot*_win64_console.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $godot) { throw "Godot not found; winget install GodotEngine.GodotEngine" }
$game = Resolve-Path "$PSScriptRoot\..\game"
$p = Start-Process -FilePath $godot.FullName -PassThru -NoNewWindow -ArgumentList @(
    "--rendering-method", "gl_compatibility", "--path", $game, "--script", "$PSScriptRoot\test_$Test.gd", "--", $OutDir)
if (-not $p.WaitForExit(240000)) { $p.Kill(); throw "timeout" }
exit $p.ExitCode
