#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Installs the .deb into a clean Debian 13 container and checks it (SPECIFICATION §7.1):
#   - apt resolves all dependencies from Debian (nothing bundled),
#   - files land in /opt/drone_simulator, with /usr/bin, desktop entry and icon,
#   - the GUI creates ~/.drone_simulation (0700) with the database (0600), and purging the
#     package leaves that user data alone,
#   - the program starts (--version) and the GUI starts on a virtual X display (xvfb) with
#     OpenGL 4.5 core from Mesa llvmpipe, compiles the installed shaders and stores the
#     shipped vehicle profiles,
#   - purge removes everything and never touches user data.
#
# Usage: scripts/ci/test-deb-install.sh build/release/drone-simulator_<ver>_amd64.deb
set -euo pipefail

deb="${1:?usage: $0 <package.deb>}"
deb_dir="$(cd "$(dirname "$deb")" && pwd)"
deb_name="$(basename "$deb")"
image="${DS_TEST_IMAGE:-debian:trixie}"

docker run --rm -i \
    -v "$deb_dir/$deb_name:/tmp/$deb_name:ro" \
    -e DEB="/tmp/$deb_name" \
    "$image" bash -euo pipefail -s <<'IN_CONTAINER'
export DEBIAN_FRONTEND=noninteractive
step() { printf '\n=== %s\n' "$*"; }

step "Install package (no recommends, to prove Depends is complete)"
apt-get update -qq
apt-get install -y -qq --no-install-recommends "$DEB" >/dev/null
dpkg -s drone-simulator | grep -E '^(Status|Version):'

step "Installed files"
test -x /opt/drone_simulator/bin/drone_simulator
test "$(readlink /usr/bin/drone-simulator)" = /opt/drone_simulator/bin/drone_simulator
test -f /usr/share/applications/drone-simulator.desktop
test -f /usr/share/icons/hicolor/scalable/apps/drone-simulator.svg
test -f /usr/share/doc/drone-simulator/copyright
test -f /usr/share/man/man6/drone-simulator.6.gz
test -f /usr/share/doc/drone-simulator/changelog.Debian.gz
test -f /usr/share/doc/drone-simulator/THIRD_PARTY_NOTICES.md
test -s /opt/drone_simulator/share/sky/bsc5.csv
test -s /opt/drone_simulator/share/sky/moon_albedo.pgm
test -s /opt/drone_simulator/share/shaders/atmosphere.glsl
test -s /opt/drone_simulator/share/shaders/terrain.frag
test -s /opt/drone_simulator/share/shaders/object.frag
test -s /opt/drone_simulator/share/vehicles/aerial/camera-250g.json
test -s /opt/drone_simulator/share/vehicles/aerial/fpv-5in-racer.json
test -s /opt/drone_simulator/share/vehicles/aerial/survey-1500g.json
test -s /opt/drone_simulator/share/environment/wind_levels.json
ldd /opt/drone_simulator/bin/drone_simulator | grep -q 'libSDL3\.so'
if ldd /opt/drone_simulator/bin/drone_simulator | grep -q 'not found'; then
    ldd /opt/drone_simulator/bin/drone_simulator; exit 1
fi
echo "ok"

step "Run --version"
out="$(drone-simulator --version)"
echo "$out"
[[ "$out" == "Drone Simulator "* ]]

step "Start the GUI on Xvfb (Mesa llvmpipe, OpenGL 4.5 core)"
apt-get install -y -qq --no-install-recommends xvfb xauth mesa-utils libgl1-mesa-dri >/dev/null
useradd -m pilot
set +e
su pilot -c 'cd ~ && xvfb-run -a -s "-screen 0 1280x800x24" timeout 10 drone-simulator --log-level debug' >/tmp/run.log 2>&1
code=$?
set -e
cat /tmp/run.log
# 124 = still running when timeout stopped it, i.e. it started and kept running.
[[ $code -eq 124 ]] || { echo "unexpected exit code $code"; exit 1; }
grep -q 'OpenGL 4\.[5-9] core' /tmp/run.log
# The installed shaders compile (they are found in /opt/drone_simulator/share).
grep -q 'Renderer resources loaded from /opt/drone_simulator/share/shaders' /tmp/run.log
# The shipped vehicle profiles reach the user's database (FR-DB-09); wind levels load.
grep -q 'Vehicle profiles: 3 added' /tmp/run.log
! grep -q 'Using the built-in wind levels' /tmp/run.log
! grep -q 'Skipping vehicle profile' /tmp/run.log

step "User database created privately (FR-DB-01)"
test "$(stat -c %a /home/pilot/.drone_simulation)" = 700
test "$(stat -c %a /home/pilot/.drone_simulation/drone_simulation.db)" = 600
test "$(stat -c %U /home/pilot/.drone_simulation/drone_simulation.db)" = pilot
grep -q 'schema v' /tmp/run.log
echo "ok"

step "Purge"
apt-get purge -y -qq drone-simulator >/dev/null
test ! -e /opt/drone_simulator
test ! -e /usr/bin/drone-simulator
test -f /home/pilot/.drone_simulation/drone_simulation.db  # User data is never removed.
test ! -e /usr/share/applications/drone-simulator.desktop
echo "ok"

echo
echo "PASS: clean install / run / purge"
IN_CONTAINER
