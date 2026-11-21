# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

ds_add_core_module(net
    SOURCES
        block_planner.cpp
        data_client.cpp
        disk_cache.cpp
        geo_rect.cpp
        http_client.cpp
        module.cpp
        net_error.cpp
        provider.cpp
        quota.cpp
        url_template.cpp
    DEPS
        ds::common
        ds::geo)

# libcurl (curl licence) stays private: its types never appear in ds/net headers (§3.1).
target_link_libraries(ds_net PRIVATE CURL::libcurl)

# Data-provider presets (FR-TER-O02), found at run time in <prefix>/share/providers.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/assets/providers/
    DESTINATION ${DS_INSTALL_DATADIR}/providers
    COMPONENT runtime
    FILES_MATCHING PATTERN "*.json")
