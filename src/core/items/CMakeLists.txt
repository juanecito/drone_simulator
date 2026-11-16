# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

find_package(glm REQUIRED CONFIG)

ds_add_core_module(items
    SOURCES
        item.cpp
        module.cpp
        registry.cpp
        triggers.cpp
    DEPS
        ds::common
        glm::glm)
