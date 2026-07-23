Author: author-kit test fixture
Ring: any
Provenance: hand-authored under tools/testdata/ to round-trip the --scene importer

This plot exists only to prove the author kit end to end: a drawable Ground layer on the pack's
TilesetFloor, a `Mark:Spawn` semantic layer marking one cell as a spawn anchor, and an `Anim:smoke`
motile that the renderer animates closed-form from the world clock. If codegen ever stops emitting
`spawn_cells` or the motile table for this prefab, the author kit has regressed.
