# Terrain Editor

Windows desktop prototype for a heightfield-oriented terrain editor.

## Current Prototype

- C++20 / CMake application
- Win32 + DirectX 12 renderer
- Dear ImGui shell UI
- imgui-node-editor backed node graph
- Internal `NodeGraph` model with nodes, pins, links, parameters, and evaluation status
- CPU dense SDF preview evaluator for primitive, noise, and crack parameters
- Split production layout:
  - left preview viewport
  - right-top node network
  - right-bottom inspector tabs for properties, stats, camera, compute, and export
- Animated cube placeholder in the viewport
- Editable prototype parameters for primitive, noise, and crack settings
- SDF preview stats for resolution, SDF range, fill ratio, and estimated volume
- Viewport overlay showing the evaluated SDF center slice
- Viewport point-cloud preview sampled near the SDF zero surface
- Debug wire preview generated from SDF sign-change cells
- Prototype triangle surface preview built from SDF sign-change quads
- GPU SDF raymarch preview toggle in `表示 > SDF Raymarch Preview`, with CPU fallback
- Debug OBJ export to `exports/terrain_debug.obj`
- Interactive viewport orbit, pan, zoom, and reset controls
- Node-stage preview: selecting graph nodes shows that generation stage in the viewport
- View menu toggles Mesh, lightweight Voxels, and GPU Raymarch previews
- Final mesh evaluation now builds indexed topology with shared vertices, triangle indices, unique edges, and vertex normals
- JSON-driven UI themes in `data/ui_themes`, selectable from `設定 > UIテーマ`
- Preview compute backend selection for CPU / GPU Preview / Auto; GPU preview uses D3D12 compute for the preview SDF values while final output remains CPU-based.
- JSON project save/load via `ファイル > 保存` / `ファイル > 開く` using `.terrainproj` files, with legacy `.rockproj` loading retained.
- App settings persistence in `data/app_settings.json` for UI theme, preview compute backend, viewport camera state, preview visibility, and up to 8 recent project files.

OpenVDB, real SDF evaluation, mesh extraction, and export nodes are intentionally left for later phases.

## Build

Install dependencies with vcpkg, then configure with the vcpkg toolchain:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

Run:

```powershell
./build/Debug/terrain_editor.exe
```
