cmake_minimum_required(VERSION 3.25)

project(TerrainEditor VERSION 0.20.0 LANGUAGES CXX RC)

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
    src/main.cpp
    src/node_graph.cpp
    src/obj_exporter.cpp
    src/screenshot_capture.cpp
    src/sdf_preview.cpp
    src/ui/UiTheme.cpp
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
        $<TARGET_FILE_DIR:terrain_editor>/data/ui_themes
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/data
        $<TARGET_FILE_DIR:terrain_editor>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:terrain_editor>/shaders
    COMMENT "Copying data files..."
)
