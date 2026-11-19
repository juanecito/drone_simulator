# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT

find_package(glm REQUIRED CONFIG)

ds_add_core_module(graphics
    SOURCES
        camera.cpp
        camera_rig.cpp
        drone_model.cpp
        rover_model.cpp
        mesh.cpp
        shader_library.cpp
        exposure.cpp
        image.cpp
        module.cpp
        renderer.cpp
        shader_source.cpp
        shadow_cascades.cpp
        sky_model.cpp
    DEPS
        ds::common
        ds::geo
        ds::astro
        ds::weather
        ds::terrain
        glm::glm)

# glad and OpenGL stay private: public headers expose no GL or glad types.
target_link_libraries(ds_graphics PRIVATE ds::glad OpenGL::GL)

# Shaders (FR-GFX-04) and the moon albedo map, found at run time in <prefix>/share.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/assets/shaders/
    DESTINATION ${DS_INSTALL_DATADIR}/shaders
    COMPONENT runtime
    FILES_MATCHING PATTERN "*.glsl" PATTERN "*.vert" PATTERN "*.frag" PATTERN "*.comp")
install(FILES ${PROJECT_SOURCE_DIR}/assets/sky/moon_albedo.pgm
    DESTINATION ${DS_INSTALL_DATADIR}/sky
    COMPONENT runtime)
