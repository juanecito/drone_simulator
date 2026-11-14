#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Installs the build, test and documentation dependencies on Debian 13 (trixie).
# Run as root (CI container) or with sudo.
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates file dpkg-dev \
    qt6-base-dev qt6-base-dev-tools libqt6opengl6-dev libqt6sql6-sqlite \
    libgl-dev libegl-dev libglm-dev libsdl3-dev nlohmann-json3-dev libcurl4-openssl-dev \
    xvfb xauth libgl1-mesa-dri \
    doxygen graphviz \
    clang-tidy clang-format lintian
