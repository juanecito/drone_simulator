# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Toolchain hardening, matching Debian's "hardening=+all" defaults (checked by lintian):
# stack protector, stack-clash protection, CET, PIE, full RELRO + immediate binding, and
# _FORTIFY_SOURCE in optimised builds (it requires optimisation and conflicts with ASan).

option(DS_ENABLE_HARDENING "Build with hardening flags" ON)

if(DS_ENABLE_HARDENING AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    include(CheckPIESupported)
    check_pie_supported()

    add_compile_options(
        -fstack-protector-strong
        -fstack-clash-protection
        $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>:-fcf-protection>)
    if(NOT DS_ENABLE_SANITIZERS)
        add_compile_options(
            $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>)
    endif()
    add_link_options(LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack)
endif()
