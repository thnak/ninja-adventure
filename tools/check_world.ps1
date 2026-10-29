# Kiểm tra đồ thị khu thật và đồ thị thử. Thoát mã 0 nếu cả hai sạch.
$code = 0
foreach ($graph in @("res://content/world_graph.json", "res://tests/world_graph_test.json")) {
    $env:WORLD_GRAPH = $graph
    & "$PSScriptRoot/godot.ps1" --script "$PSScriptRoot/check_world.gd"
    if ($LASTEXITCODE -ne 0) { $code = 1 }
}
Remove-Item Env:WORLD_GRAPH
exit $code
