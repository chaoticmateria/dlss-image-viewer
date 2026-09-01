cmake_minimum_required(VERSION 3.24)
project(DLSSImageViewer VERSION 0.11.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DLSSImageViewer currently targets Windows x64 / D3D12 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Use Visual Studio 2022 / MSVC for this build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/DLSS" CACHE PATH "Path to NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NVIDIA DLSS SDK not found at ${DLSS_SDK}. Run build_windows.bat; it clones the official NVIDIA/DLSS repository automatically.")
endif()

set(DLSS_LIB_DIR "${DLSS_SDK}/lib/Windows_x86_64/x64")
set(DLSS_RUNTIME_REL "${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
set(DLSS_RUNTIME_DEV "${DLSS_SDK}/lib/Windows_x86_64/dev/nvngx_dlss.dll")

add_executable(DLSSImageViewer WIN32
    src/resource.rc
    src/main.cpp
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/DLSSBackend.cpp
    src/DLSSBackend.h
    src/D3D12Renderer.cpp
    src/D3D12Renderer.h
    src/TemporalGuides.cpp
    src/TemporalGuides.h
    src/AudioPlayer.cpp
    src/AudioPlayer.h
    src/Log.h
    src/Localization.h
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_win32.cpp
    external/imgui/backends/imgui_impl_dx12.cpp
)

target_compile_definitions(DLSSImageViewer PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(DLSSImageViewer PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_include_directories(DLSSImageViewer PRIVATE 
    "${DLSS_SDK}/include"
    "${CMAKE_SOURCE_DIR}/external/imgui"
    "${CMAKE_SOURCE_DIR}/external/imgui/backends"
)
target_link_directories(DLSSImageViewer PRIVATE "${DLSS_LIB_DIR}")
target_link_libraries(DLSSImageViewer PRIVATE
    d3d12 dxgi dxguid d3dcompiler
    mfplat mfreadwrite mfuuid
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi
    nvsdk_ngx_d
)

add_custom_command(TARGET DLSSImageViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${DLSS_RUNTIME_DEV},${DLSS_RUNTIME_REL}>"
        "$<TARGET_FILE_DIR:DLSSImageViewer>/nvngx_dlss.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/README.md"
        "$<TARGET_FILE_DIR:DLSSImageViewer>/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:DLSSImageViewer>/LICENSE"
)

# The one-click BAT stages these binaries automatically. Keep this optional so
# developers can still configure CMake before downloading FFmpeg.
set(FFMPEG_STAGED_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin")
if(EXISTS "${FFMPEG_STAGED_DIR}/ffmpeg.exe" AND EXISTS "${FFMPEG_STAGED_DIR}/ffprobe.exe")
    add_custom_command(TARGET DLSSImageViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffmpeg.exe"
            "$<TARGET_FILE_DIR:DLSSImageViewer>/ffmpeg.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffprobe.exe"
            "$<TARGET_FILE_DIR:DLSSImageViewer>/ffprobe.exe"
    )
endif()
