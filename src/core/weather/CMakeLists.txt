# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(weather
    SOURCES
        module.cpp
        wind_levels.cpp
    DEPS
        ds::common)

# Wind levels (FR-WIND-02), found at run time in <prefix>/share/environment.
install(FILES ${PROJECT_SOURCE_DIR}/assets/environment/wind_levels.json
    DESTINATION ${DS_INSTALL_DATADIR}/environment
    COMPONENT runtime)
