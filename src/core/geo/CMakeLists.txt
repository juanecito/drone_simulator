# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(geo
    SOURCES
        coordinates.cpp
        geodesic.cpp
        module.cpp
        utm.cpp
        web_mercator.cpp
    DEPS
        ds::common)
