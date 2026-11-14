# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

configure_file(version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/include/ds/common/version.hpp @ONLY)

ds_add_core_module(common
    SOURCES
        json.cpp
        log.cpp
        module.cpp
        random.cpp
        worker_pool.cpp)

target_include_directories(ds_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>)

# nlohmann/json is part of the public interface (ds/common/json.hpp).
target_link_libraries(ds_common PUBLIC nlohmann_json::nlohmann_json)
