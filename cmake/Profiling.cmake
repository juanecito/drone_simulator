# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Profiling switch (preset "profile"): keeps the build type's optimisation (Release in the
# preset, so the code matches the .deb) and adds what sampling profilers need to produce
# readable call stacks — debug info for symbols and source lines, and frame pointers so that
# `perf record --call-graph fp` (or hotspot, heaptrack) can unwind our code cheaply.

if(DS_ENABLE_PROFILING AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-g -fno-omit-frame-pointer)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|aarch64|arm64")
        add_compile_options(-mno-omit-leaf-frame-pointer)
    endif()
endif()
