cmake_minimum_required(VERSION 3.25)

project(TerrainEditor VERSION 0.19.95 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

configure_file(
    ${CMAKE_SOURCE_DIR}/src/Version.h.in
    ${CMAKE_BINARY_DIR}/generated/Version.h
    @ONLY
)

add_executable(terrain_editor WIN32
    src/terrain_editor.rc
    src/D3D12Utils.cpp
    src/evaluation/Colorize.cpp
    src/evaluation/HeightfieldOps.cpp
    src/evaluation/HeightmapSource.cpp
    src/evaluation/MaskNoise.cpp
    src/evaluation/MaskOps.cpp
    src/evaluation/MultiScaleErosion.cpp
    src/evaluation/Rock.cpp
    src/evaluation/Sediment.cpp
    src/evaluation/ShapeSource.cpp
    src/evaluation/Snow.cpp
    src/gpu/ColorizeCompute.cpp
    src/gpu/MaskFluvialCompute.cpp
    src/gpu/MaskNoiseCompute.cpp
    src/gpu/MaskUtilityCompute.cpp
    src/gpu/MseCompute.cpp
    src/gpu/RockCompute.cpp
    src/gpu/ScatterCompute.cpp
    src/gpu/SedimentCompute.cpp
    src/gpu/SnowCompute.cpp
    src/main.cpp
    src/NodeSerialization.cpp
    src/node_graph.cpp
    src/obj_exporter.cpp
    src/PathUtils.cpp
    src/ProjectSettingsSerialization.cpp
    src/platform/FileDialogs.cpp
    src/platform/ShellUtils.cpp
    src/rendering/MeshPreviewRenderer.cpp
    src/screenshot_capture.cpp
    src/texture_exporter.cpp
    src/ui/AppFonts.cpp
    src/ui/AssetExportPanel.cpp
    src/ui/CameraPanel.cpp
    src/ui/DebugPanel.cpp
    src/ui/DisplayPanel.cpp
    src/ui/Localization.cpp
    src/ui/NodeIcon.cpp
    src/ui/NodePins.cpp
    src/ui/NodeProperties.cpp
    src/ui/PropertyWidgets.cpp
    src/ui/SkyPanel.cpp
    src/ui/UiTheme.cpp
    src/ui/WaterPanel.cpp
    src/ViewportMath.cpp
    src/ViewportSerialization.cpp
)

target_link_libraries(terrain_editor PRIVATE
    imgui::imgui
    nlohmann_json::nlohmann_json
    unofficial::imgui-node-editor::imgui-node-editor
    d3d12
    dxgi
    d3dcompiler
    comdlg32
    dwmapi
    gdi32
    ole32
    windowscodecs
)

target_include_directories(terrain_editor PRIVATE
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(terrain_editor PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(terrain_editor PRIVATE /W4 /permissive- /utf-8)
endif()

add_custom_command(TARGET terrain_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf
        $<TARGET_FILE_DIR:terrain_editor>/assets
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:terrain_editor>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:terrain_editor>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:terrain_editor>/shaders
    COMMENT "Copying runtime files..."
)
