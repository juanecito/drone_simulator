# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Debian package (SPECIFICATION §7.1): drone-simulator_<version>-<rev>_amd64.deb,
# installing into /opt/drone_simulator, with Debian 13 libraries as dependencies.
#
#   cmake --preset release && cmake --build --preset release --target package

# Debian policy: directories 0755, independent of the builder's umask.
set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

set(DS_DEB_RELEASE 1)  # Debian package revision (the "-N" in X.Y.Z-N).

if(NOT DS_PACKAGE_DESKTOP_INTEGRATION)
    # Plain installs (e.g. into a user prefix): documents stay inside the prefix.
    install(FILES
        ${CMAKE_SOURCE_DIR}/LICENSE
        ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        DESTINATION ${DS_INSTALL_DOCDIR}
        COMPONENT runtime)
else()
    # --- Debian package: files outside the prefix (absolute paths) --------------------
    set(DS_PACKAGE_PREFIX /opt/drone_simulator)
    set(pkg_dir ${CMAKE_BINARY_DIR}/packaging)
    set(doc_dir /usr/share/doc/drone-simulator)
    file(MAKE_DIRECTORY ${pkg_dir})

    # Compresses <src> into <dst>.gz reproducibly (gzip -9n), as Debian policy requires.
    function(ds_gzip src dst)
        set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
        execute_process(COMMAND gzip -9nc ${src} OUTPUT_FILE ${dst} COMMAND_ERROR_IS_FATAL ANY)
    endfunction()

    # /usr/bin/drone-simulator -> /opt/drone_simulator/bin/drone_simulator (FR-PKG-05)
    file(CREATE_LINK ${DS_PACKAGE_PREFIX}/${DS_INSTALL_BINDIR}/drone_simulator
         ${pkg_dir}/drone-simulator SYMBOLIC)
    install(FILES ${pkg_dir}/drone-simulator DESTINATION /usr/bin COMPONENT runtime)

    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/drone-simulator.desktop
        DESTINATION /usr/share/applications COMPONENT runtime)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/drone-simulator.svg
        DESTINATION /usr/share/icons/hicolor/scalable/apps COMPONENT runtime)

    # Manual page (section 6: games).
    configure_file(${CMAKE_SOURCE_DIR}/packaging/linux/drone-simulator.6
                   ${pkg_dir}/drone-simulator.6 @ONLY)
    ds_gzip(${pkg_dir}/drone-simulator.6 ${pkg_dir}/drone-simulator.6.gz)
    install(FILES ${pkg_dir}/drone-simulator.6.gz
        DESTINATION /usr/share/man/man6 COMPONENT runtime)

    # Documentation: DEP-5 copyright (contains the MIT licence), notices, Debian changelog.
    # The top changelog entry must match the package version.
    set(changelog ${CMAKE_SOURCE_DIR}/packaging/debian/changelog)
    file(STRINGS ${changelog} changelog_head LIMIT_COUNT 1)
    set(expected_head "drone-simulator (${PROJECT_VERSION}-${DS_DEB_RELEASE})")
    string(FIND "${changelog_head}" "${expected_head}" pos)
    if(NOT pos EQUAL 0)
        message(FATAL_ERROR "packaging/debian/changelog must start with an entry for "
                            "'${expected_head}', found: '${changelog_head}'")
    endif()
    ds_gzip(${changelog} ${pkg_dir}/changelog.Debian.gz)
    install(FILES
        ${CMAKE_SOURCE_DIR}/packaging/debian/copyright
        ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        ${pkg_dir}/changelog.Debian.gz
        DESTINATION ${doc_dir} COMPONENT runtime)

    # Documented lintian overrides for the deliberate /opt layout (FR-PKG-07).
    configure_file(${CMAKE_SOURCE_DIR}/packaging/debian/lintian-overrides
                   ${pkg_dir}/lintian-overrides/drone-simulator COPYONLY)
    install(FILES ${pkg_dir}/lintian-overrides/drone-simulator
        DESTINATION /usr/share/lintian/overrides COMPONENT runtime)
endif()

# --- CPack / DEB ------------------------------------------------------------------
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME drone-simulator)
set(CPACK_PACKAGE_VENDOR "Juan Maria Gomez Lopez")
set(CPACK_PACKAGE_CONTACT "Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Realistic 3D drone simulator with missions")
# Debian: extended description lines must stay under 80 characters.
set(CPACK_PACKAGE_DESCRIPTION
"Drone Simulator is a desktop application for realistic simulation of
aerial (multirotor) and ground (RC rover) drones over procedural or
real-world terrain, with mission-based play, weather and an
astronomically correct day/night sky.")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGING_INSTALL_PREFIX /opt/drone_simulator)
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_RELEASE ${DS_DEB_RELEASE})
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
set(CPACK_DEBIAN_PACKAGE_SECTION games)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER ${CPACK_PACKAGE_CONTACT})
# Shared-library dependencies are computed by dpkg-shlibdeps (Qt, OpenGL, SDL 3, libstdc++ ...).
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Dependencies dpkg-shlibdeps cannot detect (FR-PKG-04):
#  - the Qt SQLite driver is a plugin, loaded at runtime;
#  - CA certificates for HTTPS to data providers (used from milestone M4).
# tzdata: std::chrono time zones (local time and DST, FR-AST-02).
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite, ca-certificates, tzdata")
# share/scripts/fetch_mission_data.py fetches the data of the real-terrain missions.
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "python3")

include(CPack)
