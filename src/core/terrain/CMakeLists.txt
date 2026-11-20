# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(terrain
    SOURCES
        chunk.cpp
        chunk_selection.cpp
        gdal_support.cpp
        height_source.cpp
        module.cpp
        noise.cpp
        procedural_source.cpp
        query.cpp
        raster_source.cpp
        streamer.cpp
        terrain_files.cpp
    DEPS
        ds::common
        ds::geo
        ds::net)

# GDAL (MIT) stays private: its types never appear in ds/terrain headers (§3.1).
target_link_libraries(ds_terrain PRIVATE GDAL::GDAL)
