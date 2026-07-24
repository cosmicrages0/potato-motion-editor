cmake_minimum_required(VERSION 3.20)
project(MotionEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Fetch SDL2
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.28.5
)
FetchContent_MakeAvailable(SDL2)

# Fetch ImGui Docking
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
)
FetchContent_MakeAvailable(imgui)

# Task 5.2: JSON for save/load. Header-only, INTERFACE target -- only adds
# an include path, no linker cost. #included ONLY in src/Serialization.cpp
# so template instantiation bloat stays isolated to that one translation
# unit (~120KB with /GL /LTCG per external LLM analysis).
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE
    SDL2::SDL2main
    SDL2::SDL2
    d3d11
    dxgi
    d3dcompiler
    Comdlg32               # Task 5.2: Win32 GetOpenFileName/GetSaveFileName
    Dwrite                 # Task 5.9: DirectWrite text rasterization
    Shell32                # Task 5.9: SHGetKnownFolderPath for LOCALAPPDATA
    Ole32                  # Task 5.9: CoTaskMemFree for KNOWNFOLDER path result
    nlohmann_json::nlohmann_json
)

target_sources(${PROJECT_NAME} PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    src/
)
