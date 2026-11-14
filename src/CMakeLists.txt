# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

# Core libraries (Qt-free, except storage) — dependency order matters.
add_subdirectory(core/common)
add_subdirectory(core/geo)
add_subdirectory(core/astro)
add_subdirectory(core/weather)
add_subdirectory(core/net)
add_subdirectory(core/storage)
add_subdirectory(core/terrain)
add_subdirectory(core/items)
add_subdirectory(core/input)
add_subdirectory(core/physics)
add_subdirectory(core/missions)
add_subdirectory(core/graphics)
add_subdirectory(core/sim)

# Qt6 GUI application.
add_subdirectory(app)
