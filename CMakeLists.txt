cmake_minimum_required(VERSION 3.20)
project(HdrSnip LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(HdrSnip WIN32
    src/main.cpp
    src/capture.cpp
    src/tonemap.cpp
    src/overlay.cpp
    src/output.cpp
    src/preview.cpp
    src/ui.cpp
    src/share.cpp
    res/app.rc
)

target_compile_definitions(HdrSnip PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(HdrSnip PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(HdrSnip PRIVATE d3d11 dxgi windowscodecs windowsapp shlwapi dwmapi gdiplus d3dcompiler CoreMessaging comctl32)
target_link_options(HdrSnip PRIVATE /MANIFEST:NO) # res/app.manifest is embedded via app.rc
