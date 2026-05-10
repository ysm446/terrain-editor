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

The application is a single executable. `src/main.cpp` (~390 KB) owns the Win32 window, D3D12 device/swapchain, ImGui setup, file IO, viewport rendering, and all property-panel UI. `src/node_graph.{h,cpp}` owns the data model and evaluation. `src/obj_exporter.cpp` is an isolated utility.

### NodeGraph evaluation pipeline

`rock::NodeGraph` holds `nodes_`, `links_`, per-node settings, an in-memory `heightfieldCache_` keyed by node id, and a parallel `maskCache_` for mask-graph nodes. There are two evaluation tracks that meet only at the `Mask Fluvial` node:

- **Heightfield pipeline** — built by walking upstream from the target node and collecting `HeightfieldOperation`s on top of a `Heightmap Load` or `Shape` source. Current ops: `Heightmap Blur`, `Erosion Noise`, `Multi-Scale Erosion`, `Mask Fluvial`. Drives the 3D / 2D viewports and OBJ export.
- **Mask graph** — `Mask Noise`, `Mask Blend` (and `Mask Fluvial` as a bridge) live in their own pipeline rooted in `EvaluateMaskGridForNodeCached`. `Mask Fluvial` consumes a heightfield, so when the mask graph encounters one it pulls the heightfield pipeline result via `EvaluateHeightPipelineCached` and lifts `grid.mask` out as a `MaskGrid`.

Evaluation runs asynchronously: `StartAsyncEvaluation()` in `main.cpp` snapshots the graph, hands it to `std::async`, and the main thread polls the future. The current graph carries `Evaluating...` status until the result is merged via `ApplyEvaluationResultFrom`. `rock::CurrentlyEvaluatingNodeId()` exposes a thread-safe atomic that the worker stores into before each cache miss; the UI thread reads it to paint a "計算中" badge that walks the upstream chain in real time.

Caching: each operation node has its own cache entry keyed by `(input hash, parameter hash, resolution)`. Touching unrelated nodes does not re-run upstream work. Hash functions live next to each settings struct (e.g. `HashMaskFluvialSettings`, `HashMultiScaleErosionSettings`).

### Heightfield model

`HeightfieldGrid` holds heights and several auxiliary fields populated by the simulation:

| Field      | Meaning                                               |
| ---------- | ----------------------------------------------------- |
| `heights`  | The heightmap (meters, 1 unit = 1 m).                 |
| `mask`     | Generic 0–1 mask used as the visualization channel. Mask Fluvial writes its drainage map here. |
| `deposits` | Sediment deposit accumulator (Multi-Scale Erosion).   |
| `flows`    | Stream / flow accumulator (Multi-Scale Erosion).      |
| `age`      | Per-cell age (decays where the cell is reshaped).     |

`HeightfieldPreviewField` (`Heightmap` / `Deposits` / `Flows` / `Age` / `Mask`) selects which of those is copied into `mask` for visualization; the value is set when the user clicks an output pin. Add a new field by extending the enum, `HeightfieldGrid`, `SelectHeightfieldPreviewField`, and the `heightfieldFieldName` lambdas in `main.cpp`.

`MaskGrid` (`resolution`, `values`) is the parallel data type for pure mask nodes. `Mask Fluvial`'s bridge into the mask graph copies `grid.mask` out into a `MaskGrid` so downstream `Mask Blend` can mix it with `Mask Noise`.

### Multi-Scale Erosion (the load-bearing erosion node)

`ApplyMultiScaleErosion` is a CPU port of the Schott et al. SIGGRAPH 2024 shaders (`erosion.glsl` / `thermal.glsl` / `deposition.glsl`, source repo: `H-Schott/MultiScaleErosion`). The legacy KTT-based fluvial node is no longer in the codebase but historical reference material is kept under `docs/nodes/heightfield/fluvial_erosion/`.

The current node uses three coupled passes:

- **Stream Power Erosion** (D8 weighted-flow with `flow_p` exponent on slope, `speStrength * (stream^streamExp) * (slope^slopeExp)`).
- **Thermal / talus** with a 3×3 stencil (wraparound boundary, optional noise on the threshold angle).
- **Deposition** sharing the same D8 flow direction as SPE.

A multi-grid pyramid (`useMultigrid = true`) runs from `kCoarsestPyramidLevel` up to the target with bilinear upsampling between levels — drainage networks form quickly at coarse cellSize and finer levels only refine, giving near-resolution-invariant results.

### Mask Fluvial

D8 / MFD flow-accumulation node that emits a `Mask`. Heights pass through unchanged (no heightfield output pin). Algorithm: optional iterative pit-fill (Jacobi double-buffer, `ParallelForRows`), `std::execution::par` sort by descending height, then a single sequential pass that pushes each cell's accumulator to its downhill neighbour(s). Output curves: `Log` (default — continuous dendritic drainage map), `Threshold` (binary river extraction), `Linear`. The accumulation loop is inherently sequential due to the descending-height topological order; the surrounding work (sort, pit fill, mask conversion with `std::log` / `std::pow`) is parallelised.

### Sky and clouds

Optional `Atmospheric` sky mode (Nishita single-scatter Rayleigh + Mie + Henyey-Greenstein) with a Hillaire 2020 multi-scatter LUT (`shaders/atmosphere_multiscatter.hlsl`). Sun colour, terrain ambient and cloud lighting are all derived from the same model so day → sunset → night transitions stay coherent when the sun elevation slider moves.

Volumetric clouds (`shaders/cloud_render.hlsl`) raymarch a 128³ R8 density volume bounded by a cylinder (altitude slab × disc fade). Shading is `ambient + sunlit × lightTransmittance × phase`: the light transmittance comes from a short Beer-Lambert march toward the sun (`lightSamples` × `lightStepMeters`), and `phase` is a 4π-normalised HG. `lightSamples = 0` falls back to the original yNorm vertical ramp. Cloud shadows are pre-baked into a 1024² R8 from a top-down march and sampled by the terrain mesh shader.

Detailed parameter / formula reference is in `docs/sky_and_clouds/sky_and_clouds.md` — keep that file in sync when the cloud / atmosphere model changes.

### Project files

- `.terrainproj` (JSON, current) saves nodes/links/settings/preview state, including grid display configuration under the `preview` block.
- `.rockproj` (legacy) load is preserved for old samples in `data/`.
- `data/app_settings.json` persists UI theme, camera, and recent projects (no per-project visual state).
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
- `docs/nodes/README.md` — index of per-node documentation (heightfield + mask categories).
- `docs/nodes/heightfield/multi_scale_erosion/multi_scale_erosion_algorithm_guide.md` — current load-bearing erosion node's algorithm notes.
- `docs/nodes/heightfield/fluvial_erosion/` — historical KTT-based fluvial reference (no longer wired into the build).
- `docs/nodes/node_candidates.md` — backlog of node ideas.
- `docs/sky_and_clouds/sky_and_clouds.md` — sky / atmosphere / volumetric cloud reference.
- `docs/mask_texture/mask_texture.md` — mask preview rendering (3 shading modes, wall sentinel, half-Lambert hatch).
