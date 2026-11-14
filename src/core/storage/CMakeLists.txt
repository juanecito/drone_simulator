# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# The only Core module allowed to use Qt: Qt6::Core and Qt6::Sql (SPECIFICATION §3.1).

ds_add_core_module(storage
    SOURCES
        connection.cpp
        data_location.cpp
        input_profile_store.cpp
        migrations.cpp
        module.cpp
        schema.cpp
        session_store.cpp
        settings_store.cpp
        storage_error.cpp
        storage_service.cpp
        terrain_preset_store.cpp
        vehicle_profile_store.cpp
    DEPS
        ds::common
        Qt6::Core
        Qt6::Sql)
