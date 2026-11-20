# Building Drone Simulator

## Requirements (Debian 13 "trixie", amd64)

| Tool / library | Version | Debian package |
|----------------|---------|----------------|
| C++ compiler | GCC 14 or Clang 19 (C++23) | `build-essential` |
| CMake | ≥ 3.28 | `cmake` |
| Ninja | any | `ninja-build` |
| Qt | ≥ 6.4 (Debian: 6.8): Core, Gui, Widgets, OpenGL, OpenGLWidgets, Sql, Test | `qt6-base-dev`, `libqt6opengl6-dev`, `libqt6sql6-sqlite` |
| OpenGL headers | — | `libgl-dev`, `libegl-dev` |
| GLM | any | `libglm-dev` |
| SDL 3 | ≥ 3.2 (joystick subsystem only) | `libsdl3-dev` |
| Assimp | ≥ 5.2 (glTF 2.0 / OBJ model files) | `libassimp-dev` |
| libcurl | ≥ 7.85 (HTTPS to data providers) | `libcurl4-openssl-dev` |
| nlohmann/json | ≥ 3.11 (header-only; optional, see below) | `nlohmann-json3-dev` |
| Doxygen (+ Graphviz) | optional, for `docs` | `doxygen`, `graphviz` |
| lintian, clang-format | optional, package and style checks | `lintian`, `clang-format` |
| Xvfb + Mesa | optional, headless OpenGL tests | `xvfb`, `xauth`, `libgl1-mesa-dri` |

Install everything with:

```sh
sudo scripts/ci/install-deps.sh
```

GoogleTest 1.15.2 is downloaded by CMake (checksum-verified) when tests are enabled; it is
never installed or packaged. If `nlohmann-json3-dev` is not installed, CMake downloads the
pinned nlohmann/json 3.11.3 release (checksum-verified) instead; it is header-only and compiled
into the binary.

At run time the graphics driver must provide **OpenGL 4.5 core profile**.

## Presets

All builds use CMake presets; build trees go to `build/<preset>/`.

| Preset | Purpose |
|--------|---------|
| `debug` | Development build; OpenGL debug context and driver messages on. |
| `release` | Optimised build used for the `.deb` (enables desktop integration files). |
| `relwithdebinfo` | Optimised with debug symbols. |
| `asan` | Debug + AddressSanitizer + UndefinedBehaviorSanitizer. |
| `coverage` | Debug + coverage instrumentation. |
| `profile` | Release optimisation + debug info + frame pointers, for `perf` / hotspot (see [Profiling](#profiling)). |
| `ci` | Debug with compiler warnings **and** Doxygen warnings treated as errors. |

```sh
cmake --preset debug
cmake --build --preset debug
```

## Tests

```sh
ctest --preset debug                    # everything
ctest --preset debug -L unit            # unit tests only
ctest --preset debug -L integration     # GUI, OpenGL and command-line tests
```

- **Unit tests** (`tests/unit`) cover Core modules and the command-line parser. The `geo` and
  `astro` tests compare against reference data in `tests/fixtures/` (see below).
- **Storage tests** (`ds_storage_unit_tests`, `ds_storage_integration_tests`) use in-memory
  and temporary-file SQLite databases; they never touch `~/.drone_simulation`.
- **GUI tests** (`ds_gui_tests`) run on Qt's `offscreen` platform and need no display.
- **OpenGL tests** (`ds_gl_tests`) need a real display, because Qt's `offscreen` platform
  creates legacy (non-core) contexts. Without a display, or without OpenGL 4.5 core, they are
  **skipped**, not failed. On a headless machine run them with a virtual X server:

  ```sh
  xvfb-run -a -s "-screen 0 1280x800x24" ctest --preset debug
  ```

  With Mesa this uses the llvmpipe software renderer, which supports OpenGL 4.5 core.
- **Scene rendering tests** (in `ds_gl_tests`) render noon, sunset, full-moon and moonless
  scenes and a moon close-up and check image properties; set `DS_TEST_IMAGE_DIR=<dir>` to
  save the images. `SceneRenderTest.FrameTimeAt1080p` prints the frame time at 1920×1080 and
  asserts 60 FPS on hardware renderers.
- **Command-line tests** run the real executable (`--version`, `--help`, invalid options).

### Valgrind

`scripts/test/run-valgrind.sh` runs the test executables under Valgrind (memcheck by default;
`--tool helgrind` or `drd` for threading errors). Each executable runs once under Valgrind,
two at a time (`-j N`). Use a build without sanitizers: the default is `build/debug`, and
ASan builds are refused.

```sh
scripts/test/run-valgrind.sh                                  # all except ds_gl_tests
scripts/test/run-valgrind.sh -f 'MissionRunner.*' ds_unit_tests
scripts/test/run-valgrind.sh --tool helgrind ds_mission_flight_tests
scripts/test/run-valgrind.sh --gl -f 'Viewport.*' ds_gl_tests  # native OpenGL, slow
scripts/test/run-valgrind.sh --help
```

Logs go to `build/debug/valgrind/<executable>.{valgrind,test}.log`. The summary keeps
**Valgrind errors** (invalid accesses, uninitialised values, definite or indirect leaks)
apart from **test failures**. Tests run 20–50× slower under Valgrind, so a test that waits a
fixed time for the simulation can fail with no memory error at all. Known noise from system
libraries (font cache, GL drivers, D-Bus) is suppressed by `tests/valgrind.supp`. Never add
our own code there. `--gen-suppressions` writes a suppression block for every error into the
log, which helps extend the file. The script exits with 1 if anything failed.

## Profiling

The `profile` preset builds with the same optimisation as `release` (so the hot spots are
the ones users get) plus `-g` and frame pointers (`DS_ENABLE_PROFILING`). It does not enable
the desktop-integration files.

```sh
cmake --preset profile
cmake --build --preset profile

# Access to performance monitoring and observability operations is limited.
# Consider adjusting /proc/sys/kernel/perf_event_paranoid setting to open
# access to performance monitoring and observability operations for processes without CAP_PERFMON, CAP_SYS_PTRACE or CAP_SYS_ADMIN Linux capability.
# More information can be found at 'Perf events and tool security' document:
# https://www.kernel.org/doc/html/latest/admin-guide/perf-security.html
# Usually perf_event_paranoid setting is 3:

#  -1: Allow use of (almost) all events by all users
#      Ignore mlock limit after perf_event_mlock_kb without CAP_IPC_LOCK
# >= 0: Disallow raw and ftrace function tracepoint access
# >= 1: Disallow CPU event access
# >= 2: Disallow kernel profiling
# To make the adjusted perf_event_paranoid setting permanent preserve it in /etc/sysctl.conf (e.g. kernel.perf_event_paranoid = <setting>)
# sudo echo "-1" > /proc/sys/kernel/perf_event_paranoid

perf record -g -o build/profile/perf.data -- build/profile/src/app/drone_simulator
# sudo echo "3" > /proc/sys/kernel/perf_event_paranoid
perf report -i build/profile/perf.data
```

Frame pointers only cover our own code: system libraries (Qt, Mesa/NVIDIA, SDL) are built
without them, so stacks through them are cut short. Use `perf record --call-graph dwarf`
for full stacks (larger files, more overhead), or open `perf.data` in hotspot for a flame
graph. `SceneRenderTest.FrameTimeAt1080p` in `ds_gl_tests` is a repeatable render workload
to profile.

## Shaders and data files at run time

The program looks for `shaders/` and `sky/` in `$DS_ASSETS_DIR`, then in `<executable>/../share`
(the installed layout), then in the source tree's `assets/` (development builds). Debug builds
reload shaders that change on disk about once a second; a compile error is logged with the file
and line, and the previous shader stays in use.

## Documentation

```sh
cmake --build --preset debug --target docs
xdg-open build/debug/docs/html/index.html
```

The API reference is generated from the Doxygen comments in the public headers; the Markdown
pages (`README.md`, `docs/*.md`) are included. The `ci` preset fails on any Doxygen warning.

## Debian package

```sh
cmake --preset release
cmake --build --preset release
cmake --build --preset release --target package
# → build/release/drone-simulator_<version>-1_amd64.deb
```

The package:

- installs the program in `/opt/drone_simulator` (`bin/`, `share/shaders/` GLSL shaders,
  `share/sky/` star catalogue and moon texture, `share/doc/`),
- adds `/usr/bin/drone-simulator`, a desktop entry and an icon,
- depends on the Debian Qt 6 and system libraries (computed by `dpkg-shlibdeps`) plus
  `libqt6sql6-sqlite`, `ca-certificates` and `tzdata`; nothing is bundled,
- never creates, changes or removes user data in `$HOME/.drone_simulation`.

Install and remove:

```sh
sudo apt install ./build/release/drone-simulator_0.1.0-1_amd64.deb
sudo apt purge drone-simulator
```

Check it against Debian policy (the deliberate deviations — `/opt`, section `games` with the
command in `/usr/bin` — are documented in `packaging/debian/lintian-overrides`):

```sh
lintian --fail-on error,warning --info build/release/drone-simulator_0.1.0-1_amd64.deb
```

Before changing the version, add an entry to `packaging/debian/changelog` (CMake checks that
its first entry matches `<version>-<revision>`).

Check it in a clean Debian 13 container (needs Docker) — installs without recommended
packages, checks the files, starts the GUI on Xvfb and purges:

```sh
scripts/ci/test-deb-install.sh build/release/drone-simulator_0.1.0-1_amd64.deb
```

## Reference data and generated tables

The checked-in reference data and tables are produced by Python scripts. They only need to be
re-run when changing the reference set or the truncation of the series. Run them in a
virtual environment:

```sh
python3 -m venv .venv && . .venv/bin/activate
pip install pyproj geographiclib astropy
python3 scripts/fixtures/gen_geo_fixtures.py     # tests/fixtures/geo/*.csv (PROJ, GeographicLib)
python3 scripts/fixtures/gen_astro_fixtures.py   # tests/fixtures/astro/*.csv (JPL Horizons,
                                                 # USNO API, astropy; needs network)
python3 scripts/data/gen_astro_tables.py VSOP87D.ear moonposition.go src/core/astro
python3 scripts/data/gen_bsc5.py bsc5.dat.gz assets/sky/bsc5.csv
pip install Pillow
python3 scripts/data/gen_moon_texture.py lroc_color_poles_1k.jpg assets/sky/moon_albedo.pgm
```

Each script's header names the exact source files and URLs it reads. Accuracy targets
(NFR-REAL-10): Sun ≤ 0.01°, Moon ≤ 0.05°, rise/set ≤ 1 min (Sun) and 2 min (Moon) against
USNO, lunar phases ≤ 2 min, star apparent places ≤ 2″.

## Code style and checks

```sh
clang-format -i $(find src tests -name '*.cpp' -o -name '*.hpp')   # .clang-format
run-clang-tidy -p build/debug 'src/.*'               # .clang-tidy
reuse lint                                           # licensing metadata (REUSE.toml, LICENSES/)
                                                     # (outside git, run it on a copy without build/)
```

Every source file starts with SPDX headers (`MIT`, copyright of the author).
Core modules (`src/core/*`) must not use Qt — only `storage` may link Qt Core and Qt SQL; CMake
stops with an error if this rule is broken.

## Continuous integration

`.github/workflows/ci.yml` builds with the `ci` preset in a `debian:trixie` container, runs all
tests under Xvfb, generates the documentation, builds the `.deb`, test-installs it in a clean
container and runs `reuse lint`.
