# Chạy Godot (console, headless) với các tham số truyền vào, giới hạn 120 giây. Dùng cho check_world.
$godot = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Depth 2 -Filter "Godot*_win64_console.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $godot) { throw "Godot not found; winget install GodotEngine.GodotEngine" }
$game = Resolve-Path "$PSScriptRoot\..\game"
$p = Start-Process -FilePath $godot.FullName -PassThru -NoNewWindow -ArgumentList (@("--headless", "--path", $game) + $args)
if (-not $p.WaitForExit(120000)) { $p.Kill(); throw "timeout" }
exit $p.ExitCode
