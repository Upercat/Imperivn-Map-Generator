# Included generator modules

`iberivm/src/` contains only the header dependency closure needed by this utility and its tests,
copied from the independent, modularized and optimized implementation used for the 0.2.3 beta.
The files retain their original namespaces, implementation comments and technical provenance.
The original sibling development repositories were not changed or copied wholesale.

These headers include deterministic generation phases, game-data readers and supporting
simulation/data structures. No game executable, archive, settlement library, bitmap, save,
replay, capture, research database or external analysis project is bundled. Runtime data must
come from the user's own compatible installation.

The generator is compiled from these local headers. Another private checkout or submodule
is not required. `source-manifest.json` records SHA-256 digests of the imported modules.
No new license or ownership claim is assigned by collecting these source files.

Git normalizes text line endings to LF. Manifest hashes use LF-normalized bytes so they
remain comparable across Windows and other checkout configurations.
