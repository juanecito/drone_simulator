# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# ds_set_warnings(<target>): project warning flags (NFR-MNT-04).
# Applied to project targets only — never to vendored or third-party code.

function(ds_set_warnings target)
    set(flags
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        # C++20 designated initialisers deliberately omit members that have default member
        # initialisers; GCC would warn about every such use.
        -Wno-missing-field-initializers)
    if(DS_WARNINGS_AS_ERRORS)
        list(APPEND flags -Werror)
    endif()
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${flags}>)
endfunction()
