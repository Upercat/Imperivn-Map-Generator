# Deterministic generation phases

The compatibility entry point is `vendor/iberivm/src/sim/randommap/rm_generate.hpp`.
`Generator::run()` executes the pipeline; `advance()` executes one phase and exposes the
following phase through `next_phase()`. All phases receive one stable `GenerationState&`.

| Phase group | Main outputs |
| --- | --- |
| Prepare | Resolved races, template raster/markers, teams, anchors and initial field seed. |
| Landscape | Macrozones, field A, settlement objects and influence. |
| Objects | Tree/object placement, terrain classes, decoration and updated field A. |
| Terrain | Heights, lighting, terrain paint and shipyards. |
| Output | Passability, filtered objects, final grids and object XML. |

Preserve traversal order, arithmetic precision, RNG calls and input installation data when
changing a phase. Seeds alone do not identify a map: template, size, teams, races, flags,
season and game-start route also matter. Separate per-phase random streams would change maps.
The context owns intermediate grids without copying them at phase boundaries and must not move.
Use the orchestrator to enforce ordering; partial failed runs cannot be resumed.

Production generation omits diagnostic serialization/RNG history and borrows installation caches
sequentially. It can reject later saved-game candidates once final terrain differs, while retaining
the first complete candidate for mismatch fallback. Separate contexts may be interleaved; shared
installation caches are not claimed to be thread-safe. These changes preserve compatibility;
structural refactoring does not establish new provenance or an independently invented algorithm.

`generation_pipeline`, `generation_optimization`, `gallery` and `gallery_matrix` exercise these
contracts with compatible local game data. The four included JSON cases contain generation
settings only, not game assets, native dumps or captured reports.
