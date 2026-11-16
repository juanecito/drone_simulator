# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(missions
    SOURCES
        catalog.cpp
        flight_rules.cpp
        mission.cpp
        mission_loader.cpp
        mission_runner.cpp
        mission_world.cpp
        module.cpp
    DEPS
        ds::common
        ds::geo
        ds::astro
        ds::terrain
        ds::items
        ds::physics)

# Mission schema and built-in missions (§4.9.1), found at run time in <prefix>/share/missions.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/assets/missions/
    DESTINATION ${DS_INSTALL_DATADIR}/missions
    COMPONENT runtime)
