# Terrain Editor

Windows desktop prototype for a heightfield-oriented terrain editor.

## Current Prototype

- C++20 / CMake application
- Win32 + DirectX 12 renderer
- Dear ImGui shell UI
- imgui-node-editor backed node graph
- Internal `NodeGraph` model with nodes, pins, links, parameters, and evaluation status
- Heightmap image loading node with scale, relative vertical scale, and offset controls
- Split production layout:
  - left preview viewport
  - right-top node network
  - right-bottom inspector tabs for properties, stats, camera, and export
- Heightfield terrain mesh preview with 1 unit = 1 meter
- Interactive viewport orbit, pan, zoom, and reset controls
- Node-stage preview: selecting graph nodes shows that generation stage in the viewport
- View menu toggles Mesh and wireframe previews
- Final mesh evaluation now builds indexed topology with shared vertices, triangle indices, unique edges, and vertex normals
- JSON-driven UI themes in `data/ui_themes`, selectable from `設定 > UIテーマ`
- JSON project save/load via `ファイル > 保存` / `ファイル > 開く` using `.terrainproj` files, with legacy `.rockproj` loading retained.
- App settings persistence in `data/app_settings.json` for UI theme, viewport camera state, preview visibility, and up to 8 recent project files.

Terrain editing tools, layer operations, and export nodes are intentionally left for later phases.

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
