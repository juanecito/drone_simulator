# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# "docs" target: Doxygen API reference + Markdown pages (README.md as main page, docs/*.md).
# Output: <build>/docs/html/index.html

option(DS_DOCS_WARN_AS_ERROR "Fail the docs build on Doxygen warnings" OFF)

find_package(Doxygen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
    if(DS_DOCS_WARN_AS_ERROR)
        set(DS_DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)
    else()
        set(DS_DOXYGEN_WARN_AS_ERROR NO)
    endif()
    if(TARGET Doxygen::dot)
        set(DS_DOXYGEN_HAVE_DOT YES)
    else()
        set(DS_DOXYGEN_HAVE_DOT NO)
    endif()
    set(DS_DOXYGEN_OUTPUT_DIR ${CMAKE_BINARY_DIR}/docs)
    configure_file(${CMAKE_SOURCE_DIR}/docs/Doxyfile.in ${CMAKE_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(docs
        COMMAND Doxygen::doxygen ${CMAKE_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen → ${DS_DOXYGEN_OUTPUT_DIR}/html"
        VERBATIM)
else()
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo "Doxygen not found — install it with: sudo apt install doxygen graphviz"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
endif()
