# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(terrain
    SOURCES
        chunk.cpp
        chunk_selection.cpp
        height_source.cpp
        module.cpp
        noise.cpp
        procedural_source.cpp
        query.cpp
        streamer.cpp
    DEPS
        ds::common
        ds::geo
        ds::net)
