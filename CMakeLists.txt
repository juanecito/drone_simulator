# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.28)

project(DroneSimulator
    VERSION 0.1.0
    DESCRIPTION "Realistic 3D drone simulator (aerial and ground drones) with mission-based play"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(DS_BUILD_TESTS "Build unit and integration tests" ON)
option(DS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DS_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(DS_ENABLE_COVERAGE "Build with coverage instrumentation" OFF)
option(DS_ENABLE_PROFILING "Build with debug info and frame pointers for sampling profilers (perf, hotspot)" OFF)
option(DS_PACKAGE_DESKTOP_INTEGRATION
    "Install desktop entry, icon, /usr/bin symlink and DEP-5 copyright (absolute paths; for the .deb package)" OFF)

include(GNUInstallDirs)
include(CompilerWarnings)
include(Sanitizers)
include(Profiling)
include(Hardening)
include(CoreModules)

# Installed layout (relocatable): <prefix>/bin, <prefix>/lib, <prefix>/share/...
# The app finds its data relative to its own executable (<bin>/../share).
set(DS_INSTALL_BINDIR bin)
set(DS_INSTALL_LIBDIR lib)
set(DS_INSTALL_DATADIR share)
set(DS_INSTALL_DOCDIR share/doc)
set(CMAKE_INSTALL_RPATH "$ORIGIN/../${DS_INSTALL_LIBDIR}")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Sql)
find_package(OpenGL REQUIRED COMPONENTS OpenGL)

add_subdirectory(third_party/glad)

# nlohmann/json (MIT, header-only; Core JSON: vehicle profiles, wind levels, input mappings).
# Debian's nlohmann-json3-dev is used when installed, otherwise the pinned upstream release.
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
        SYSTEM)
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# SDL3 (zlib licence): RC transmitters and gamepads (FR-IN-01), used only inside core/input.
find_package(SDL3 3.2 REQUIRED CONFIG COMPONENTS SDL3-shared)

# libcurl (curl licence, MIT-style): HTTP(S) to data providers (§4.6), used only inside core/net.
find_package(CURL 7.85 REQUIRED)

# GDAL (MIT, brings PROJ): elevation rasters and orthophotos (§4.5.4, §4.5.5), used only inside
# core/terrain; GDAL types never appear in ds/terrain headers.
# GDAL's package version file only accepts the same minor version, so check the minimum here.
find_package(GDAL CONFIG REQUIRED)
if(GDAL_VERSION VERSION_LESS 3.6)
    message(FATAL_ERROR "GDAL ${GDAL_VERSION} found; 3.6 or newer is required")
endif()

# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(DS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

include(Doxygen)
include(Packaging)

ds_check_core_qt_policy()
