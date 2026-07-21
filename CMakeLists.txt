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

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_link_libraries(${PROJECT_NAME} PRIVATE 
    SDL2::SDL2main
    SDL2::SDL2 
    d3d11 
    dxgi 
    d3dcompiler
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
