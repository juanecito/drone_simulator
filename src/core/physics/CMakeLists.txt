# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

find_package(glm REQUIRED CONFIG)

ds_add_core_module(physics
    SOURCES
        aerial_profile.cpp
        aerial_vehicle.cpp
        aerodynamics.cpp
        atmosphere.cpp
        drive_controller.cpp
        estimator.cpp
        failures.cpp
        flight_controller.cpp
        frames.cpp
        ground_profile.cpp
        ground_vehicle.cpp
        module.cpp
        powertrain.cpp
        rigid_body.cpp
        sensors.cpp
        surface.cpp
        vehicle_factory.cpp
        wind.cpp
    DEPS
        ds::common
        ds::geo
        ds::terrain
        ds::items
        ds::weather
        glm::glm)

# Reference vehicle profiles (FR-PHY-16), found at run time in <prefix>/share/vehicles.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/assets/vehicles/
    DESTINATION ${DS_INSTALL_DATADIR}/vehicles
    COMPONENT runtime
    FILES_MATCHING PATTERN "*.json")
