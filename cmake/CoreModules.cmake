# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Helpers for Core modules (src/core/<name>).
#
#   ds_add_core_module(<name> SOURCES <src...> [DEPS <targets...>])
#
# Creates the static library ds_<name> (alias ds::<name>) with public headers in
# src/core/<name>/include (included as "ds/<name>/...").
#
#   ds_check_core_qt_policy()
#
# Enforces NFR-MNT-01: Core modules must not link Qt, except ds_storage, which may
# link Qt6::Core and Qt6::Sql only.

define_property(GLOBAL PROPERTY DS_CORE_TARGETS
    BRIEF_DOCS "All Core module targets"
    FULL_DOCS "Targets created with ds_add_core_module().")

function(ds_add_core_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
    set(target ds_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(ds::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_compile_features(${target} PUBLIC cxx_std_23)
    if(ARG_DEPS)
        target_link_libraries(${target} PUBLIC ${ARG_DEPS})
    endif()
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    ds_set_warnings(${target})
    set_property(GLOBAL APPEND PROPERTY DS_CORE_TARGETS ${target})
endfunction()

function(ds_check_core_qt_policy)
    get_property(targets GLOBAL PROPERTY DS_CORE_TARGETS)
    foreach(target IN LISTS targets)
        set(libs "")
        foreach(prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(value ${target} ${prop})
            if(value)
                list(APPEND libs ${value})
            endif()
        endforeach()
        foreach(lib IN LISTS libs)
            if(lib MATCHES "^Qt[56]?::")
                if(target STREQUAL "ds_storage" AND lib MATCHES "^Qt6::(Core|Sql)$")
                    continue()
                endif()
                message(FATAL_ERROR
                    "Core module '${target}' links '${lib}'. Core must stay Qt-free "
                    "(only ds_storage may link Qt6::Core and Qt6::Sql) — see SPECIFICATION §3.1.")
            endif()
        endforeach()
    endforeach()
endfunction()
