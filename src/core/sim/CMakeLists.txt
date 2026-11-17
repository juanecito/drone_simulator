# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(sim
    SOURCES
        fixed_step.cpp
        hud.cpp
        item_visuals.cpp
        mission_session.cpp
        module.cpp
        simulation_loop.cpp
        world.cpp
    DEPS
        ds::common
        ds::geo
        ds::astro
        ds::weather
        ds::storage
        ds::net
        ds::terrain
        ds::items
        ds::input
        ds::physics
        ds::missions
        ds::graphics)
