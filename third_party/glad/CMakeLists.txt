# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# glad 2.0.8 loader for OpenGL 4.5 core + GL_KHR_debug, generated with:
#   glad --api "gl:core=4.5" --extensions GL_KHR_debug --out-path . c
# Generated files are (WTFPL OR CC0-1.0) AND Apache-2.0; khrplatform.h is MIT-style (Khronos).

add_library(ds_glad STATIC src/gl.c)
add_library(ds::glad ALIAS ds_glad)
target_include_directories(ds_glad SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(ds_glad PROPERTIES
    C_STANDARD 11
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ds_glad PUBLIC ${CMAKE_DL_LIBS})
