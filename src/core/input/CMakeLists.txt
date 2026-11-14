# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(input
    SOURCES
        calibration.cpp
        input_selector.cpp
        joystick_service.cpp
        keyboard.cpp
        mapping.cpp
        module.cpp
    DEPS
        ds::common)

# SDL3 stays private: no SDL type appears in the public headers (SPECIFICATION §3.1).
target_link_libraries(ds_input PRIVATE SDL3::SDL3)
