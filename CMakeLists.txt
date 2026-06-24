cmake_minimum_required(VERSION 3.15)
project(MediaShow2 LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ASLR/DEP/Manifest are set via target_link_options below (more reliable with VS generator)

add_library(MediaShow2 SHARED
    src/dllmain.cpp
    src/mf_player.cpp
    src/ds_player.cpp
    src/resources.rc
)

target_include_directories(MediaShow2 PRIVATE
    src
    sdk
)

target_compile_definitions(MediaShow2 PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    WLX_EXPORTS
)

target_link_libraries(MediaShow2 PRIVATE
    comctl32
    shell32
    ole32
    oleaut32
    winmm
    shlwapi
    dwmapi
    uxtheme
)

target_compile_options(MediaShow2 PRIVATE
    /W3
    /GS
)

# ASLR (/DYNAMICBASE), DEP (/NXCOMPAT), suppress auto-manifest (resources.rc provides it)
target_link_options(MediaShow2 PRIVATE
    /DYNAMICBASE
    /NXCOMPAT
    /MANIFEST:NO
)

set_target_properties(MediaShow2 PROPERTIES
    OUTPUT_NAME "MediaShow2"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(MediaShow2 PROPERTIES
        OUTPUT_NAME "MediaShow2_x64"
    )
endif()
