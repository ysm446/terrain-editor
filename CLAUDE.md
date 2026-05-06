# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Windows desktop prototype of a node-based terrain editor centered on heightfields. C++20, Win32 + Direct3D 12, Dear ImGui (with imgui-node-editor), nlohmann_json. Originally a rock generator; pivoted to terrain (heightfield IO + procedural processing). 1 unit = 1 m throughout.

## Build, run, version

Dependencies are managed via vcpkg (`vcpkg.json`). Configure with the vcpkg toolchain:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
./build/Debug/terrain_editor.exe
```

The post-build step copies `data/` and `shaders/` next to the exe; expect `terrain_editor.exe` to be **locked while running** — close it before rebuilding (the link step fails with `LNK1168` otherwise).

There is no test framework, lint task, or CI configured. Verification is manual through the UI.

**Version policy (from `AGENTS.md`)** — applies to *every* user-visible change:

1. Bump `project(TerrainEditor VERSION x.y.z ...)` in `CMakeLists.txt` (patch / minor / major per impact).
2. Add an entry under `## 未リリース` in `docs/changelog.md`, **in Japanese**.
3. When releasing, promote `## 未リリース` to a `## x.y.z - YYYY-MM-DD HH:MM` heading.
4. Source files must read the version from the generated `Version.h` (built from `src/Version.h.in`); do not duplicate version strings.

## Architecture

The application is a single executable. `src/main.cpp` (~270 KB) owns the Win32 window, D3D12 device/swapchain, ImGui setup, file IO, viewport rendering, and all property-panel UI. `src/node_graph.{h,cpp}` owns the data model and evaluation. `src/obj_exporter.cpp` is an isolated utility.

### NodeGraph evaluation pipeline

`rock::NodeGraph` holds `nodes_`, `links_`, per-node settings, and an in-memory `heightfieldCache_` keyed by node id. The graph is evaluated through a preview pipeline built around `HeightfieldPipeline`:

- **Preview pipeline** — built from the currently selected node back to the source. Drives the 3D and 2D viewports. Runs asynchronously: `StartAsyncEvaluation()` in `main.cpp` snapshots the graph, hands it to `std::async`, and the main thread polls the future. The current graph carries `Evaluating...` status until the result is merged via `ApplyEvaluationResultFrom`.
- **OBJ export** — writes the currently evaluated preview mesh.

`HeightfieldPipeline` is built from the graph by walking upstream from the target node. It collects heightfield operations layered on top of a `Heightmap Load` or `Shape` source.

- **Heightfield chain** (`HeightfieldOperation`): heightfield ops (`Heightmap Blur`, `Erosion Noise`, `Multi-Scale Erosion`) layered on top of a `Heightmap Load` or `Shape` source.

Caching: `BuildMeshFromHeightPipelineCached` caches each heightfield operation's output by (input hash, parameter hash, resolution). Touching unrelated nodes does not re-run upstream work. Hash functions live next to each settings struct (e.g. `HashFluvialSettings`).

### Heightfield model

`HeightfieldGrid` holds heights and several auxiliary fields populated by the simulation:

| Field      | Meaning                                               |
| ---------- | ----------------------------------------------------- |
| `heights`  | The heightmap (meters, 1 unit = 1 m).                 |
| `mask`     | Generic 0–1 mask used as the visualization channel.   |
| `deposits` | Sediment deposit accumulator from fluvial erosion.    |
| `flows`    | Particle visit count from fluvial erosion.            |
| `age`      | Per-cell age (decays where the cell is reshaped).     |

`HeightfieldPreviewField` (`Heightmap` / `Deposits` / `Flows` / `Age`) selects which of those is copied into `mask` for visualization; the value is set when the user clicks one of a node's output pins. Add a new field by extending the enum, `HeightfieldGrid`, `SelectHeightfieldPreviewField`, and the `heightfieldFieldName` lambdas in `main.cpp`.

### Fluvial Erosion (the load-bearing node)

`ApplyFluvialErosionSingleLevel` is a CPU port of the KTT_Fluvial_Erosion HDA (the OpenCL kernels `Fluvial_Sim_Test` for forces and `Fluvial_Sim` for transport). When working on this node, treat the HDA as the spec. Several non-obvious details matter:

- Particle height read/write goes through `sampleHeight` and `splatField` (4-tap bilinear), **not** `floor(px)/floor(pz)`. The naive nearest-cell write breaks mirror symmetry around `(n-1)/2` and produces a directional bias that compounds across iterations.
- `referenceDetailSize` is the KTT `Detail_Scale`; `stepScale = cellSize / detailScale` drives both step count and per-step velocity gain. When `stepScale` is large (i.e. coarse simulation cells with `detailScale ≈ 1m`), particles teleport and the per-cell modifications become rectangular blocks. The multi-level pyramid (`kFluvialLevelStrengths` / `levelResolutions`) currently zeroes out the 16/32/64 levels for this reason — the active levels are 128, 256, 512.
- Angles are compared in *tangent* space. `Wear/Deposit/MaxAngle` are converted via `tan(deg * π/180)` and compared against `length(force)`.
- `flowVolume` binds to `Update_Forces`'s `Flow_Cutting`, not anything in `Smooth_Flows`. It scales the `wear * Flow_Cutting` feedback into the gradient and only activates when `dx <= Detail_Scale`.

The detailed mapping between HDA parameters and the CPU implementation is in `docs/fluvial_erosion_hda_notes.md` — keep that file in sync whenever the kernel parity changes. The HDA itself is at `ref/KTT/Kruger.KTT_Fluvial_Erosion.1.0.hda` (gzip-compressed CPIO inside).

### Project files

- `.terrainproj` (JSON, current) saves nodes/links/settings/preview state.
- `.rockproj` (legacy) load is preserved for old samples in `data/`.
- `data/app_settings.json` persists UI theme, camera, and recent projects.
- UI themes are JSON files under `data/ui_themes/`, switchable from `設定 > UIテーマ`.

## Conventions

- Comments and changelog entries are in Japanese; UI strings are mostly Japanese.
- The codebase is in the `rock::` namespace (legacy from the rock-generator era).
- MSVC flags `/W4 /permissive- /utf-8`. Save sources as **UTF-8 without BOM** — `AGENTS.md` documents the PowerShell snippets used to enforce this.
- `WIN32_LEAN_AND_MEAN` and `NOMINMAX` are defined globally; assume `<windows.h>` will not pollute `min`/`max`.
- The reference KTT HDA collection in `ref/KTT/` is git-ignored input material, not part of the build.

## Pointers

- `README.md` — feature overview (Japanese).
- `AGENTS.md` — versioning rules and the UTF-8 read/write recipes.
- `docs/changelog.md` — required for every user-visible change.
- `docs/fluvial_erosion_hda_notes.md` — HDA-vs-CPU parity reference.
- `docs/fluvial_erosion_node.md` — implementation notes for the node.
- `docs/node_candidates.md` — backlog of node ideas.
