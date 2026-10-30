cmake_minimum_required(VERSION 3.21)

project(dock
    VERSION 0.1.0
    DESCRIPTION "Hold WIN+Space to peek at a folder's contents over any window"
    HOMEPAGE_URL "https://github.com/Mesrine67/dock"
    LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "dock targets Windows only (Win32 + Direct2D).")
endif()

# Default to Release for single-config generators (NMake, Ninja, Makefiles).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

option(DOCK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Version reported by the in-app update check. CI overrides this with the pushed
# tag (e.g. -D DOCK_VERSION=0.2.0) so an installed build knows its own version.
if(NOT DEFINED DOCK_VERSION)
    set(DOCK_VERSION "${PROJECT_VERSION}")
endif()

add_executable(dock WIN32)

target_sources(dock PRIVATE
    src/main.cpp
    src/App.cpp
    src/FolderModel.cpp
    src/FolderWatcher.cpp
    src/KeyboardHook.cpp
    src/OverlayWindow.cpp
    src/Settings.cpp
    src/SettingsWindow.cpp
    src/UpdateChecker.cpp
    src/app.rc
    src/app.manifest)

target_include_directories(dock PRIVATE src)

target_compile_features(dock PRIVATE cxx_std_17)

target_compile_definitions(dock PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    DOCK_VERSION="${DOCK_VERSION}")

if(MSVC)
    target_compile_options(dock PRIVATE /W4 /permissive- /EHsc /utf-8)
    if(DOCK_WARNINGS_AS_ERRORS)
        target_compile_options(dock PRIVATE /WX)
    endif()
endif()

# Windowing, Direct2D / DirectWrite / WIC rendering, and Shell integration.
target_link_libraries(dock PRIVATE
    d2d1
    dwrite
    windowscodecs
    shell32
    shlwapi
    ole32
    user32
    gdi32
    dwmapi
    uxtheme
    comctl32
    shcore
    winhttp
    bcrypt)
