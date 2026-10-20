# Imperivm Map Generator

A Windows utility for recovering maps from Imperivm HD replays/saved games and generating
a selectable 3x3 gallery of deterministic maps. Current version: **0.2.3 beta candidate**.

## Download and use

[Download the Windows x64 ZIP](downloads/ImperivmMapExtractor-0.2.3-beta-windows-x64.zip).
Extract it before running `imperivm-map-extractor.exe`. The executable requires Windows 10/11
x64 and your own unmodified Imperivm HD installation. Python, Visual Studio and a separate
C++ runtime installation are not required. Windows 11 was tested; separate Windows 10
acceptance is still pending. The executable is unsigned.

- **Recover maps:** open/drop a `.dmo` replay or `.snr` saved game, review verification and export.
- **Generate maps:** choose template, size, players, civilisation and a hexadecimal base seed.
  Generate nine maps, select a thumbnail, or double-click it to view it across the window.
  Use **Back to grid** or **Esc** to return. The window can be resized/maximized.
- **Save / install:** export the selected map as `.BFHP` or add it to the chosen game's `Scenarios`
  folder. Existing scenario files are not overwritten. If installation is denied, save elsewhere.

The same seed, settings and installation reproduce the same map. Each card uses the base seed
plus its zero-based index; Next 9 maps advances the base seed by nine. Completed maps remain
available after stopping a batch. Switching tabs retains recovery and gallery independently.

Full instructions: [user guide](docs/USER_GUIDE.txt). Changes: [changelog](CHANGELOG.txt).
Archive integrity: [SHA-256](downloads/SHA256SUMS.txt).

## Scope and current limitations

- Requires a separately installed game. This repository's downloadable ZIP includes no game
  executable, packs, saves, replays or generated maps.
- Random-map recovery only; unsupported builds, modified installations, campaigns and predefined
  maps can be refused. Replays contain no terrain against which to verify the recovered map.
- Generated maps currently use free-for-all settings and spring. A fixed civilisation applies
  to all active players, or random resolves independently for each player.
- Eight compatible templates are offered. Tournament is omitted because its bitmap format is
  unsupported by the current generator.
- Preferences use `%LOCALAPPDATA%\ImperivmMapExtractor\settings.ini`; older portable settings
  are read for compatibility. No personal configuration is bundled.
- Closing during a long recovery may wait for its worker. Multiplayer, clean-machine and current
  native-game acceptance remain pending; this is a beta candidate rather than a stable release.

## Validation

The candidate passed 16/16 automated checks, including deterministic generation fixtures,
19 compatible template/size variants, failed-write preservation, cancellation, navigation,
malformed-file refusals and exports matching an independent generation route. Three additional
smoke checks passed using the executable extracted into a different directory with spaces and
non-ASCII characters. This coverage does not test every seed or replace native-game acceptance.

## Feedback and distribution

Report problems through this repository's Issues with tool/Windows/game version and, for
generation, template, size, player count, civilisation and seed. Avoid attaching private player
data unnecessarily. Application and generator source code are included. Distribution/license terms remain the
author's decision; no open-source license has been assigned by this initial preparation.

## Build from source

The application lives in `src/`; the included header dependency closure of the modular generator
lives in `vendor/iberivm/src/`. No separate private repository or submodule is required.
Use a Visual Studio **x64 Native Tools** prompt with C++20 tools, a Windows SDK and CMake 3.20+:

```bat
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

This runs the standalone checks without needing game files. Python 3 enables the executable
strings check. Integration checks are opt-in: set `MAPX_GAME_ROOT` to your own installation.
Set `MAPX_REPLAY_FIXTURE` to a replay for the UI suite, or `MAPX_SAVE_FIXTURE_A` and
`MAPX_SAVE_FIXTURE_B` to verified/mismatch saves for optimization equivalence. Frozen external
fixture lists are optional via `MAPX_FIXTURE_BASE` and `MAPX_FIXTURE_MANIFEST_DIR`; private
fixture file names and all actual game/replay/save data are deliberately excluded from Git.

The compiler uses a static C++ runtime. The application is built as `build/imperivm-map-extractor.exe`.
Console probes, tests and benchmarks are developer tools and are not in the downloadable ZIP.
Generator phase contracts: [generation architecture](docs/GENERATION.md).
