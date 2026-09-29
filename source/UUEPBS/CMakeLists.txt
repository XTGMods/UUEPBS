# UUEPBS - native DLL
#
# Standalone project: it does NOT build against RE-UE4SS, so no UE4SS checkout,
# no UEPseudo submodule and no Epic/GitHub account link are needed. The DLL is
# loaded by Scripts/main.lua with package.loadlib and talks to it through files,
# which keeps it working across UE4SS releases.
#
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# Output: build/Release/UUEPBS.dll
cmake_minimum_required(VERSION 3.22)
project(UUEPBS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL has no runtime dependencies beyond Windows itself.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build for x64 (add -A x64): Unreal games are 64-bit.")
endif()

# Dear ImGui with the Win32 + DirectX 11 backends. Use -DIMGUI_DIR=<path> for an
# existing checkout (offline builds); otherwise it is downloaded once.
set(IMGUI_DIR "" CACHE PATH "Existing Dear ImGui checkout (optional)")
if(IMGUI_DIR STREQUAL "")
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR ${imgui_SOURCE_DIR})
endif()

add_library(uuepbs_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(uuepbs_imgui PUBLIC ${IMGUI_DIR})
target_compile_definitions(uuepbs_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

add_library(UUEPBS SHARED
    src/dllmain.cpp
    src/core/body_groups.cpp
    src/core/bridge.cpp
    src/core/mirror.cpp
    src/core/presets.cpp
    src/core/registry.cpp
    src/core/resolver.cpp
    src/core/rig_names.cpp
    src/core/sculpt.cpp
    src/ui/panel_view.cpp
    src/ui/soft_raster.cpp
    src/win/pose_hook.cpp
    src/win/slider_window.cpp)

target_include_directories(UUEPBS PRIVATE src)
target_compile_definitions(UUEPBS PRIVATE UNICODE _UNICODE)
target_link_libraries(UUEPBS PRIVATE uuepbs_imgui d3d11 dxgi dwmapi shell32 ole32 user32)
if(MSVC)
    target_compile_options(UUEPBS PRIVATE /W4 /permissive- /utf-8 /O2)
    target_compile_options(uuepbs_imgui PRIVATE /utf-8)
    # Direct3D is only loaded if config.lua asks for Renderer = "gpu". With the default CPU
    # renderer these DLLs are never mapped by this module, so graphics hooks cannot reach the window.
    target_link_options(UUEPBS PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:d3dcompiler_47.dll /IGNORE:4199)
    target_link_libraries(UUEPBS PRIVATE delayimp)
endif()

# Keep the file where the build says it is: build/<Config>/UUEPBS.dll
set_target_properties(UUEPBS PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")

# Optional: copy into a game after every build.
#   cmake -B build ... -DUUEPBS_GAME_WIN64="D:/Steam/steamapps/common/<Game>/<Project>/Binaries/Win64"
if(DEFINED UUEPBS_GAME_WIN64)
    add_custom_command(TARGET UUEPBS POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${UUEPBS_GAME_WIN64}/ue4ss/Mods/UUEPBS/native"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:UUEPBS> "${UUEPBS_GAME_WIN64}/ue4ss/Mods/UUEPBS/native/UUEPBS.dll")
endif()
