# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Qt6 Widgets application (SPECIFICATION §3.1, §4.11).
#   ds_app_ui       — static library with the GUI (MainWindow, Viewport, Settings tab,
#                     settings adapter, start-up storage, command line);
#                     linked by the executable and by the integration tests.
#   drone_simulator — the executable (main.cpp only).

add_library(ds_app_ui STATIC
    asset_paths.cpp
    command_line.cpp
    controls_panel.cpp
    environment_panel.cpp
    hud_painter.cpp
    scene_config.cpp
    scene_controller.cpp
    terrain_panel.cpp
    opengl_check.cpp
    main_window.cpp
    mission_dialogs.cpp
    mission_library.cpp
    items_controller.cpp
    items_panel.cpp
    mission_setup.cpp
    mission_validation.cpp
    missions_controller.cpp
    missions_panel.cpp
    settings_controller.cpp
    settings_panel.cpp
    simulation_controller.cpp
    simulation_panel.cpp
    user_storage.cpp
    vehicle_panel.cpp
    viewport.cpp
    include/ds/app/asset_paths.hpp
    include/ds/app/command_line.hpp
    include/ds/app/controls_panel.hpp
    include/ds/app/environment_panel.hpp
    include/ds/app/hud_painter.hpp
    include/ds/app/scene_config.hpp
    include/ds/app/scene_controller.hpp
    include/ds/app/terrain_panel.hpp
    include/ds/app/items_controller.hpp
    include/ds/app/items_panel.hpp
    include/ds/app/main_window.hpp
    include/ds/app/mission_dialogs.hpp
    include/ds/app/mission_library.hpp
    include/ds/app/mission_setup.hpp
    include/ds/app/mission_validation.hpp
    include/ds/app/missions_controller.hpp
    include/ds/app/missions_panel.hpp
    include/ds/app/opengl_check.hpp
    include/ds/app/settings_controller.hpp
    include/ds/app/settings_panel.hpp
    include/ds/app/simulation_controller.hpp
    include/ds/app/simulation_panel.hpp
    include/ds/app/user_storage.hpp
    include/ds/app/vehicle_panel.hpp
    include/ds/app/viewport.hpp)
add_library(ds::app_ui ALIAS ds_app_ui)
set_target_properties(ds_app_ui PROPERTIES AUTOMOC ON)
target_include_directories(ds_app_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ds_app_ui PUBLIC
    ds::common
    ds::geo
    ds::astro
    ds::terrain
    ds::graphics
    ds::storage
    ds::weather
    ds::input
    ds::physics
    ds::items
    ds::missions
    ds::sim
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets)
target_compile_definitions(ds_app_ui PRIVATE
    DS_SOURCE_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
ds_set_warnings(ds_app_ui)

qt_add_executable(drone_simulator main.cpp)
target_link_libraries(drone_simulator PRIVATE ds::app_ui)
ds_set_warnings(drone_simulator)

install(TARGETS drone_simulator
    RUNTIME DESTINATION ${DS_INSTALL_BINDIR} COMPONENT runtime)
