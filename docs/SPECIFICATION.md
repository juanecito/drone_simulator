# Drone Simulator — Software Specification

| Field          | Value                         |
|----------------|-------------------------------|
| Document       | Software Requirements & Design Specification |
| Version        | 0.2 (draft)                   |
| Date           | 2026-11-13                    |
| Status         | Proposed — pending review     |
| Source         | `CLAUDE.md` (project brief) + clarifications from the product owner |
| License        | MIT — Copyright (c) 2026 Juan Maria Gomez Lopez (see §12) |

**Changes since 0.1:** product goal set to *realism + mission-based play*; Missions
module added; Terrain Generator supports *procedural*, *online elevation (DEM, web APIs)*
and *elevation files referenced by the mission JSON* (**COG** and other raster formats, and
**LAS/LAZ** lidar point clouds); **orthophotos from files defined in the mission JSON**
(online imagery providers later); Network &
Data Cache module added; UTM coordinates and drone elevation always shown on the main screen;
geo-referencing module added; flight model, sensors and RC input upgraded for realism;
terrain dimensions made optional (set by the user or by a mission); MIT licensing
analysis added (§12); **wind levels 0–10** added (§4.2.1); **ground drone (UGV)** that only drives added (§4.2.2); missions defined by self-contained **JSON files** with a published JSON Schema (§4.9.1);
**date/time with astronomically correct sun, moon and stars, day/night rendering** (§4.7.1);
**weather generator** described in the mission JSON (§4.2.3); **OpenTopography** as the first
elevation provider (§4.6); **`.deb` package installing to `/opt/drone_simulator`** (§7.1); schema, build, tests and milestones updated accordingly.

---

## 1. Introduction

### 1.1 Purpose
This document defines the functional and non-functional requirements, architecture,
module boundaries, data model, build, test and documentation strategy for
**Drone Simulator**, a desktop application for real-time 3D drone flight simulation.

### 1.2 Product goal
- **Two kinds of drone.** An **aerial drone** (multirotor, flies) and a **ground drone**
  (UGV — wheeled or tracked, only drives on the terrain, never flies). Both share the same
  worlds, missions, sensors, battery model, controls setup and UI.
- **Realism first.** Flight and driving dynamics, motors, battery, atmosphere, wind and sensors are
  physically modelled so that skills and intuitions transfer to real drones.
  Where realism and convenience conflict, realism wins by default; convenience aids
  (keyboard flight, time scaling, invulnerability) are available only in free flight.
- **Play by missions.** The main way to play is a set of missions with objectives,
  constraints, failure conditions and scoring (e.g. precision landing, waypoint
  navigation, delivery, inspection, area survey, gate courses). Free flight is
  available as a sandbox.
- **Real or random worlds.** Terrain is either generated randomly (procedural, seeded)
  or built from real elevation data (**DEM**) — either fetched from a configured web API
  (OpenTopography first) or **loaded from elevation files named in the mission JSON**
  (**COG** and other raster formats, or **LAS/LAZ** lidar point clouds).
  Optional **orthophotos** are loaded from **image files defined in the mission JSON**
  (online imagery services may be added later).

### 1.3 Scope
The application lets the user fly a simulated multirotor, or drive a simulated ground
drone, over procedural or real-world
terrain under a rendered sky, play missions, configure drones, terrain and world items,
and persist configuration, mission progress and flight history in a SQLite database
belonging to the operating-system user who runs the application.

Out of scope for v1.0: multiplayer, networking, real flight-controller hardware/software
in the loop (HIL/SITL), fixed-wing aircraft, and mobile/web targets.

### 1.4 Definitions

| Term | Meaning |
|------|---------|
| **Tick** | One fixed-timestep simulation update. |
| **Frame** | One rendered image. |
| **Item** | Any entity placed in the world (drone, obstacle, gate, waypoint, landing pad, zone…). |
| **Chunk** | A square tile of terrain generated and rendered as a unit. |
| **Height source** | Provider of terrain elevation: *procedural*, *online DEM* or *DEM file*. |
| **DEM** | Digital Elevation Model: a raster of elevations (delivered by an API as GeoTIFF or encoded PNG tiles, or read from a COG file). |
| **COG** | Cloud-Optimized GeoTIFF: a GeoTIFF with internal tiling and overviews, efficient for partial reads. |
| **LAS / LAZ** | ASPRS lidar point-cloud format (LAZ = losslessly compressed LAS). Points carry x, y, z, classification (e.g. 2 = ground, 6 = building) and return information. |
| **DTM / DSM** | Digital Terrain Model (bare ground) / Digital Surface Model (top surface incl. buildings and trees). |
| **Data provider** | A configured web API (URL template + protocol + credentials + terms) delivering elevation or imagery. |
| **UTM** | Universal Transverse Mercator: zone + hemisphere/band, easting and northing in metres. |
| **CRS** | Coordinate Reference System of a geo-referenced raster (e.g. EPSG:4326, UTM). |
| **ENU** | Local East-North-Up tangent plane anchored at the scene's geographic origin. |
| **AGL / AMSL** | Altitude above ground level / above mean sea level. |
| **Vehicle** | The simulated drone: **aerial** (UAV, multirotor) or **ground** (UGV, wheeled/tracked, drive only). "Drone" in this document means either, unless stated. |
| **Scene** | Terrain source + environment + items + drone spawn/home point. |
| **Mission** | A scene plus objectives, constraints, failure conditions and scoring. |
| **Objective** | A measurable task inside a mission (reach waypoint, land, deliver…). |
| **Core** | All non-GUI code. Core does not depend on Qt, except `core/storage`, which uses Qt SQL (see §3.1). |

Acronyms and abbreviations are listed in **Appendix A**.

### 1.5 Requirement notation
Requirements carry IDs (`FR-xxx` functional, `NFR-xxx` non-functional) and a priority:
**M** = must (v1.0), **S** = should, **C** = could (later).

---

## 2. Technology Constraints

| Area | Choice | Notes |
|------|--------|-------|
| Language | **C++23** | GCC ≥ 13 or Clang ≥ 17. `-std=c++23`, warnings as errors in CI. |
| Graphics | **OpenGL ≥ 4.5 Core Profile** | DSA API, GLSL 450, reversed-Z depth. |
| GUI | **Qt 6** (Widgets) | Graphical layer only — see §3.1. |
| Build | **CMake ≥ 3.28 + Ninja** | `CMakePresets.json` for Debug / Release / ASan / Coverage. |
| Persistence | **SQLite 3** via **Qt SQL** (`QSQLITE` driver) | One DB for the OS user running the app, at `$HOME/.drone_simulation/`. |
| Geospatial I/O | **GDAL ≥ 3.6** (with PROJ) | Reading DEM files and decoding API payloads (GeoTIFF, PNG, JPEG, WebP), CRS handling and reprojection. Only an allow-list of raster drivers is enabled; GDAL never opens URLs itself (its network access is disabled). |
| Point clouds | **PDAL ≥ 2.6** (with LASzip / laz-perf) | Reading LAS/LAZ (and COPC) lidar files, filtering by classification, rasterizing to a DEM. In Debian (`libpdal-dev`). |
| HTTP client | **libcurl** (multi interface, TLS via OpenSSL/GnuTLS) | All network traffic to data providers, in `core/net`. |
| RC / gamepad input | **SDL3** (joystick + gamepad subsystems only) | Qt 6 has no gamepad module; SDL keeps input in Core, Qt-free. |
| JSON | nlohmann/json + json-schema-validator (pboettch, MIT) | Mission files (validated against a JSON Schema), profile parameters, `*_json` DB columns. |
| Math | GLM | Vectors, matrices, quaternions. `double` for geo, `float` for rendering. |
| GL loader | glad (GL 4.5 core) | Keeps the engine independent of Qt's GL wrappers. |
| Tests | GoogleTest + CTest | Unit and integration suites. |
| Docs | Doxygen + Markdown | Generated by a `docs` CMake target. |
| Primary platform | Linux (x86-64, X11/Wayland) | Windows is a *should* target; no platform-specific code in Core. |

C++23 features expected to be used: `std::expected` for error handling,
`std::print`/`std::format`, `std::mdspan` for height grids, `std::flat_map` where
appropriate, deducing `this`, `constexpr` math helpers.

---

## 3. Architecture

### 3.1 Guiding principle: "Qt only graphical"
Qt is confined to the **presentation layer** (windows, widgets, tab menus, keyboard/mouse
event capture, and providing the OpenGL context). Simulation, rendering logic, terrain,
missions and item management live in **Core libraries that do not link against Qt**.
This keeps Core unit-testable without a GUI and makes the GUI replaceable.

- **Exception — persistence:** `core/storage` uses the **Qt SQL module**
  (`QSqlDatabase` / `QSqlQuery` with the bundled `QSQLITE` driver) and therefore links
  `Qt6::Core` and `Qt6::Sql` (never `Qt6::Gui`/`Qt6::Widgets`).
- Qt types stay **inside** `core/storage`: its public API uses only standard C++ and
  domain types (`std::string`, `std::vector`, `std::expected`, structs from
  `core/common`). Conversion `QString ⇄ std::string`, `QVariant ⇄ T` happens in the
  storage implementation files, so other Core modules remain Qt-free.
- Threads, timers and file I/O in the other Core modules use the C++ standard library.
- The Graphics Engine receives a GL proc-address function from the UI layer and
  loads entry points via glad; it never includes Qt headers.
- GDAL and PDAL types stay inside `core/terrain`; libcurl types inside `core/net`; SDL types
  inside `core/input`.
- **Network:** the only network traffic is to the user-configured elevation/imagery
  providers, through `core/net` (§4.6).

### 3.2 Layered view

```
┌───────────────────────────────────────────────────────────────────┐
│ app/            Qt6 Widgets: MainWindow, Viewport, Tab panels      │ ← Qt GUI
├───────────────────────────────────────────────────────────────────┤
│ core/sim        Main Event Loop, world orchestration, snapshots    │
│ core/missions   Mission definitions, objectives, scoring           │
│ core/physics    Vehicle models (aerial + ground), atmosphere, wind,│
│                 battery, sensors, controllers, collision           │
│ core/input      RC transmitter / gamepad (SDL3), action mapping    │ ← SDL3
│ core/items      Items Manager (entity registry, spatial index)     │
│ core/terrain    Terrain: procedural, online DEM, COG + LAS/LAZ     │ ← GDAL, PDAL
│ core/net        HTTP client, data providers, tile/area disk cache  │ ← libcurl
│ core/graphics   Graphics Engine (OpenGL 4.5)                       │
│ core/weather    Seeded weather generator (clouds, rain, fog, …)    │
│ core/astro      Sim clock ⇄ UTC, sun/moon/star/planet positions    │
│ core/geo        WGS84 ⇄ ECEF ⇄ local ENU, UTM, Web Mercator        │
│ core/storage    Qt SQL (QSQLITE) persistence, migrations           │ ← Qt6::Sql
│ core/common     Math, logging, config, events, errors, JSON        │
└───────────────────────────────────────────────────────────────────┘
```

Dependency rules:
- `common` depends on nothing internal; `geo` depends only on `common`; `astro` → `geo`, `common`;
  `weather` → `common`.
- `net` → `common` (defines a `CacheIndex` interface implemented by `storage`).
- `terrain` → `net`, `geo`, `common`. `items` → `common`. `physics` → `terrain`, `items`, `geo`, `weather`.
- `missions` → `items`, `physics` (read-only state + events), `geo`.
- `graphics` → `common`, `geo`, `astro`, `weather`, `terrain` (chunk meshes and layout, read
  only; never `physics`/`missions`; it renders snapshots).
- `sim` composes everything below it. `app` is the only target linking Qt GUI modules;
  `storage` is the only Core target linking Qt (`Core` + `Sql`).

### 3.3 Threading model
| Thread | Responsibility |
|--------|----------------|
| GUI / render thread (Qt main) | Qt event loop, widget updates, OpenGL rendering of the latest simulation snapshot, GPU uploads. |
| Simulation thread | Fixed-timestep loop: input → sensors → vehicle controller → physics → collision → items → missions → snapshot publish. |
| Worker pool (N = hw threads − 2, min 1) | Terrain chunk generation (procedural noise, windowed reads from COG files, or decoding + reprojecting downloaded DEM/imagery), **lidar rasterization** (LAS/LAZ → DEM, run once per input and cached), mesh building. |
| Network thread | libcurl multi loop: prioritised, cancellable, rate-limited requests; writes the disk cache. |
| Storage thread | Owns the Qt SQL connections (user DB and cache index); executes queued DB requests (§4.10). |

Simulation ↔ render data exchange uses a **triple-buffered snapshot**
(lock-free publish/acquire), so neither thread blocks the other.
GDAL reads local DEM files and decodes downloaded payloads from memory (`/vsimem/`);
GDAL dataset handles are never shared between threads (one per worker). Physics and
rendering never wait on the network or on file I/O.

### 3.4 Repository layout

```
drone_simulator/
├── CMakeLists.txt
├── CMakePresets.json
├── CLAUDE.md
├── README.md
├── cmake/                  # toolchain helpers, warnings, sanitizers, Doxygen
├── docs/                   # Markdown docs + Doxyfile.in
├── assets/
│   ├── shaders/            # *.vert, *.frag, *.comp (GLSL 450)
│   ├── textures/
│   ├── models/             # drone and item meshes (glTF/OBJ)
│   ├── vehicles/           # reference profiles: aerial/ and ground/ (JSON)
│   ├── environment/        # wind_levels.json, weather_presets.json
│   ├── sky/                # star catalogue (BSC5), moon textures, ΔT table
│   ├── missions/           # built-in mission pack (*.mission.json) + mission.schema.json
│   ├── default-missions/   # missions copied into the user's mission folder (FR-MIS-01b)
│   └── providers/          # built-in data-provider presets (JSON)
├── src/
│   ├── app/                # Qt6 GUI (executable: drone_simulator)
│   └── core/
│       ├── astro/
│       ├── common/
│       ├── geo/
│       ├── graphics/
│       ├── input/
│       ├── items/
│       ├── missions/
│       ├── net/
│       ├── physics/
│       ├── sim/
│       ├── storage/
│       ├── terrain/
│       └── weather/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── fixtures/           # tile/coverage payloads for the mock HTTP server, missions, DB
└── third_party/            # glad (+ glm, gtest, nlohmann/json via FetchContent or system)
```

---

## 4. Module Specifications

### 4.1 Main Event Loop (`core/sim`)

**Responsibility:** Own the simulation clock and orchestrate per-tick updates.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-LOOP-01 | Run the simulation at a fixed timestep (default **500 Hz**, configurable 240–2000 Hz) using an accumulator, independent of render frame rate. | M |
| FR-LOOP-02 | Clamp the maximum catch-up per frame (default 250 ms) to avoid the "spiral of death". | M |
| FR-LOOP-03 | Support states: `Stopped`, `Running`, `Paused`, `SingleStep`. | M |
| FR-LOOP-04 | Time scaling (0.1× – 4×) in free flight/drive only; **locked to 1× during missions**. | S |
| FR-LOOP-04a | The simulation clock is tied to a real **UTC date/time**: `UTC = start UTC + elapsed simulated time`. Pausing stops it; time scaling scales it; the sun, moon and stars move continuously (e.g. the sun can set during a long mission). | M |
| FR-LOOP-05 | Per-tick order: poll input → update sensors → vehicle controller (flight or drive) → physics integrate → collision resolve → items update → missions update → publish snapshot. | M |
| FR-LOOP-06 | Publish an immutable `WorldSnapshot` (drone state, items, telemetry, mission state) after each tick. | M |
| FR-LOOP-07 | Internal event bus (type-safe, `std::variant`-based): `Collision`, `Crash`, `GatePassed`, `ZoneEntered/Exited`, `PayloadAttached/Released`, `ObjectiveCompleted/Failed`, `MissionSucceeded/Failed`, `BatteryWarning`, `Rollover`, `VehicleStuck`, `WaterEntered`, `SimReset`. | M |
| FR-LOOP-08 | Runnable headless (no GUI, no GL) for tests and batch runs. | M |

**Key interface (illustrative):**
```cpp
class SimulationLoop {
public:
    void start(); void pause(); void resume(); void step(); void stop();
    void setTickRate(double hz);
    void setTimeScale(double scale);                 // rejected while a mission runs
    [[nodiscard]] std::shared_ptr<const WorldSnapshot> latestSnapshot() const;
    EventBus& events();
};
```

### 4.2 Vehicle Models, Sensors & Controllers (`core/physics`)

All vehicles implement a common `Vehicle` interface (rigid body, battery, sensors,
controller, collision shape, telemetry), so the loop, missions, cameras, HUD and storage
work the same for both types. The requirements in this table are for the **aerial drone**;
wind levels are in §4.2.1 and the **ground drone** in §4.2.2.

**Responsibility:** Physically plausible multirotor dynamics and the onboard
flight-control stack.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-PHY-01 | 6-DoF rigid body (position, quaternion orientation, linear/angular velocity, full inertia tensor) integrated with **RK4**. Positions stored in `double` so precision holds on unbounded or large terrains. | M |
| FR-PHY-02 | Motor model: brushless motor with `KV`, winding resistance and no-load current; RPM limited by battery voltage under load; first-order spin-up/down time constant. | M |
| FR-PHY-03 | Propeller model: thrust `T = C_T·ρ·n²·D⁴`, torque `Q = C_Q·ρ·n²·D⁵`; coefficients from profile, optionally as tables vs. advance ratio for forward-flight thrust loss. | M |
| FR-PHY-04 | Aerodynamics: per-axis body drag (`½ρ·C_d·A·v²`), rotor induced drag/H-force, **ground effect** (thrust gain near ground), **vortex-ring state** thrust loss in fast vertical descent. | M (drag, ground effect) / S (H-force, VRS) |
| FR-PHY-05 | Atmosphere: ISA-based air density, pressure and temperature vs. altitude AMSL, adjusted by the weather's ground temperature, pressure (QNH) and humidity (§4.2.3); thrust drops in hot, humid or high-altitude conditions. | M |
| FR-PHY-06 | Wind: mean wind (speed, direction) with logarithmic altitude profile, **Dryden turbulence** (MIL-F-8785C), discrete gusts and direction variability, all set by a **wind level 0–10** (§4.2.1) or by custom values; terrain-shielding / ridge effects optional. | M (levels, mean, Dryden, gusts) / C (terrain effects) |
| FR-PHY-07 | Battery: LiPo/Li-ion pack (cells, capacity, internal resistance, OCV-vs-SoC curve, C-rating); voltage sag under load; current from motor model; **battery temperature** (from ambient temperature plus self-heating) changes usable capacity and internal resistance — cold batteries sag more and run out sooner; low-voltage/critical warnings; flight ends when depleted. | M |
| FR-PHY-08 | Payload: attach/release a mass at an offset (changes mass, CoG, inertia, drag) — used by delivery missions. | M |
| FR-PHY-09 | Collision with terrain (heightfield) and items (AABB / sphere / OBB / capsule); crash decided by impact speed and orientation; prop-strike damages motors (reduced thrust). | M (collision, crash) / S (damage) |
| FR-PHY-10 | Sensors: IMU (gyro + accelerometer with noise, bias, bias drift, vibration from motor RPM), barometer (noise, ground-effect error), magnetometer, GPS (5–10 Hz, position/velocity noise, satellite count, HDOP, acquisition time). Parameters per profile. | M (IMU, baro, GPS) / S (mag) |
| FR-PHY-11 | State estimation: the flight controller uses **estimated** state from sensors (complementary filter for attitude, baro/GPS fusion for altitude/position), not ground truth. EKF optional. | M (complementary) / C (EKF) |
| FR-PHY-12 | Flight controller: cascaded PID (angle → rate), D-term filtering, motor mixer for X-quad with **airmode**; RC rate/super-rate/expo curves. | M |
| FR-PHY-13 | Flight modes: **Acro**, **Angle**, **Altitude hold** (baro), **Position hold** (GPS), **Return-to-home** (S), automatic **waypoint autopilot** (C). | M / S / C |
| FR-PHY-14 | Arming with pre-arm checks (throttle low, level-ish, GPS fix required for GPS modes, battery OK); failsafe on RC signal loss (land / RTH). | M |
| FR-PHY-15 | Failure injection (used by missions): motor failure/degradation, GPS loss/drift, compass interference, battery cell fault, RC link loss. | S |
| FR-PHY-16 | Aerial drone profiles: all physical, motor, prop, battery, sensor and PID parameters. Ship **3 generic reference profiles** — **5″ FPV racer**, **sub-250 g camera drone**, **~1.5 kg survey quad** — built from typical published component specs (motors, props, batteries), not modelled on any specific commercial product or brand. | M |
| FR-PHY-17 | Numerical safety: NaN/Inf detection, state reset, event emitted. | M |

#### 4.2.1 Wind levels (0–10)

Wind is set with a single **wind level from 0 (calm) to 10 (storm)** plus a direction.
Each level expands into the full set of wind parameters below, based on the
**Beaufort scale** (forces 0–10) so the levels match real-world forecasts.

| Level | Name | Mean wind at 10 m AGL | Gust peak | Turbulence | Direction swing | Expected effect (typical drones) |
|------:|------|----------------------|-----------|------------|-----------------|----------------------------------|
| 0 | Calm | 0 m/s (0 km/h) | — | none | — | Perfectly still air. |
| 1 | Light air | 1.0 m/s (4 km/h) | 1.5 m/s | very light | ±30° | Barely noticeable drift. |
| 2 | Light breeze | 2.5 m/s (9 km/h) | 3.5 m/s | light | ±25° | Slow drift, easy to correct. |
| 3 | Gentle breeze | 4.5 m/s (16 km/h) | 6.5 m/s | light | ±20° | Constant small corrections needed. |
| 4 | Moderate breeze | 6.7 m/s (24 km/h) | 9.5 m/s | moderate | ±20° | Visible tilt to hold position; sub-250 g drones struggle in gusts. |
| 5 | Fresh breeze | 9.4 m/s (34 km/h) | 13 m/s | moderate | ±15° | Near the limit of sub-250 g drones; battery drains faster. |
| 6 | Strong breeze | 12.3 m/s (44 km/h) | 17 m/s | moderate–severe | ±15° | Camera drones near their limit; landing is hard. |
| 7 | Near gale | 15.5 m/s (56 km/h) | 21 m/s | severe | ±15° | Only powerful drones can hold position. |
| 8 | Gale | 19.0 m/s (68 km/h) | 26 m/s | severe | ±10° | Only FPV racers / heavy-lift drones; large drift. |
| 9 | Strong gale | 22.6 m/s (81 km/h) | 31 m/s | severe | ±10° | Beyond the capability of most drones. |
| 10 | Storm | 26.5 m/s (95 km/h) | 36 m/s | extreme | ±10° | Practically uncontrollable. |

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-WIND-01 | Wind level selector **0–10** (integer) plus wind direction ("from", degrees true) or *Random direction*. Level 0 means no wind, gusts or turbulence at all. | M |
| FR-WIND-02 | Each level sets: mean wind speed at 10 m AGL, gust peak, gust frequency (mean interval from ~30 s at level 1 down to ~8 s at level 10), turbulence intensity and direction swing, as in the table above. Values live in a data file (`assets/environment/wind_levels.json`) so they can be tuned without code changes. | M |
| FR-WIND-03 | Wind speed varies with height above ground (logarithmic profile, roughness from the terrain material): weaker near the ground, stronger higher up. | M |
| FR-WIND-04 | Turbulence uses the Dryden model (MIL-F-8785C, low altitude) driven by the level's wind speed; gusts are discrete "1 − cosine" gusts; the direction slowly wanders within the level's swing. All randomness uses the session seed (deterministic, FR-X-05). | M |
| FR-WIND-05 | **Effect on flight:** the drone flies relative to the moving air. Drag is computed from airspeed (drone velocity minus wind velocity), and the propellers' inflow, thrust and induced drag change with airspeed. Visible consequences: drift downwind, the tilt needed to hold position, harder take-off/landing in gusts, higher power use and shorter flight time, and loss of position hold when the wind exceeds the drone's capability. For **ground drones** wind only adds aerodynamic drag (small effect on speed and power; can matter for tall, light rovers at high levels). | M |
| FR-WIND-06 | Flight modes react realistically: Acro/Angle drift with the wind; Altitude hold and Position hold compensate only within the drone's thrust and tilt limits. | M |
| FR-WIND-07 | Each drone profile shows its **maximum wind resistance** (the level it can still hold position in, computed from max tilt, thrust and drag) in the Drone tab. The mission briefing warns when the wind level exceeds it. | M |
| FR-WIND-08 | **Custom** mode: users can set mean speed, gusts, turbulence and swing directly (Environment tab → Advanced); the nearest equivalent level is displayed. | S |
| FR-WIND-09 | Missions set wind inside the weather generator (`environment.weather.wind`: `{ "level": 0–10, "fromDeg": … }` or custom values) and can change it over time through the weather timeline (§4.2.3). | M (fixed level) / S (changes over time) |
| FR-WIND-10 | Wind is visible in the world: windsock items, swaying trees/grass, and moving clouds (where rendered). | S |
| FR-WIND-11 | Terrain effects: stronger wind on ridges and summits, turbulence on the lee side of hills and buildings. | C |

#### 4.2.2 Ground drone (UGV — drive only)

The ground drone is an **RC-style rover** (hobby-scale, battery-electric, radio-controlled,
roughly 1:10–1:8 scale, 2–6 kg). It stays on the terrain: it drives, climbs and descends slopes, and can tip
over or get stuck, but it has no flight capability.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-GND-01 | Drive configurations of RC-style rovers: **4-wheel skid-steer** (M, primary); **6-wheel skid-steer / rocker-bogie** and **Ackermann-steered RC crawler** (S); **tracked RC rover** (C); car-sized UGVs and legged robots out of scope. | M / S / C |
| FR-GND-02 | Chassis as a 6-DoF rigid body (same RK4 integrator as aerial), so it pitches, rolls and bounces over terrain. | M |
| FR-GND-03 | **Suspension:** per wheel spring–damper with travel limits; wheel–ground contact found by ray/shape queries against the terrain height field and items (ramps, obstacles). Tracks modelled as several contact points along each track. | M |
| FR-GND-04 | **Tyre/track–ground model:** longitudinal and lateral slip with a simplified Pacejka (Magic Formula) or brush model; friction and rolling resistance depend on the **terrain material** (asphalt/rock, grass, dirt, sand, snow, mud). | M |
| FR-GND-05 | **Drivetrain:** electric motor(s) per wheel or per side with torque–speed curve, gear ratio and efficiency; current drawn from the shared battery model (FR-PHY-07); braking; regenerative braking (S). | M |
| FR-GND-06 | **Terrain limits:** maximum climbable slope follows from traction and power (wheels slip and the drone slides back on slopes that are too steep); step/obstacle climbing limited by wheel radius and ground clearance; chassis bottoming out on sharp crests. | M |
| FR-GND-07 | **Rollover:** tips over when the stability limit is exceeded (steep side slopes, fast turns); a rolled-over drone cannot continue (`Rollover` event). | M |
| FR-GND-08 | **Stuck detection:** no progress despite throttle for N seconds (`VehicleStuck` event). | M |
| FR-GND-09 | **Water:** entering water deeper than the profile's wading depth stops the drone (`WaterEntered` event). | M |
| FR-GND-10 | **Sensors:** IMU, GPS, magnetometer and **wheel encoders (odometry)**, with the same noise models as the aerial drone. | M |
| FR-GND-11 | **Drive modes:** *Manual* (throttle + steering; skid-steer mixed as arcade or tank style), *Speed control* (cruise at set speed), *Heading hold* (S), *Waypoint autopilot* (C); traction control (S). Arming and failsafe (stop on RC signal loss) as for aerial drones. | M / S / C |
| FR-GND-12 | **Profiles:** mass, CoG height, wheelbase, track width, wheel radius/width, suspension stiffness/damping/travel, tyre parameters, motors, gearing, battery, wading depth, sensors, controller gains. Ship reference RC rover profiles built from published hobby-component specs: **4WD skid-steer rover ~3 kg** (M); **6WD rocker-bogie rover ~5 kg** and **1:10 Ackermann RC crawler ~2.5 kg** (S). | M / S |
| FR-GND-13 | Derived stats shown in the Vehicle tab: top speed, acceleration 0 → top, maximum climbable slope per material, static rollover angle, estimated range. | M |

#### 4.2.3 Weather generator (`core/weather`)

Weather is produced by a **seeded weather generator** whose settings are written in the
mission JSON (`environment.weather`). The same seed and settings always give the same
weather, so missions stay fair and repeatable (FR-X-05).

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-WX-01 | **Generator input:** `seed`, a base `preset`, optional overrides of any parameter, and an optional `timeline` of changes. Presets: `clear`, `few-clouds`, `scattered-clouds`, `broken-clouds`, `overcast`, `mist`, `fog`, `drizzle`, `light-rain`, `heavy-rain`, `light-snow`, `heavy-snow`, `thunderstorm` (S). Presets are data (`assets/environment/weather_presets.json`). | M |
| FR-WX-02 | **Parameters:** ground temperature (°C), humidity (%) or dew point, pressure QNH (hPa), wind (level 0–10 + direction, §4.2.1), up to 3 **cloud layers** (type: cumulus, stratocumulus, stratus, cirrus, cumulonimbus; cover in oktas 0–8; base and top in m AGL), **precipitation** (none / drizzle / rain / snow / sleet; light / moderate / heavy or mm/h), **visibility** (m), **fog** (ground fog with top height, or haze). | M |
| FR-WX-03 | **Evolution in time:** the weather varies smoothly from the seed — clouds drift with the wind at their height, cloud cover fluctuates around its set value, gusts follow the wind level. Timeline entries `{ "atS", "transitionS", preset/overrides }` blend smoothly into new conditions (e.g. rain arriving after 4 minutes). | M |
| FR-WX-04 | **Variation in space:** cloud cover and showers are not uniform; rain falls from moving shower cells, visibility can differ across the map. | S |
| FR-WX-05 | **Plausibility checks:** the mission validator rejects or warns about impossible combinations (e.g. snow at 25 °C, fog at 30 % humidity, thunderstorm with calm wind, cloud base below fog top). | M |
| FR-WX-06 | **Effects on aerial drones:** air density from temperature, pressure and humidity (thrust, FR-PHY-05); barometer reads relative to QNH (Altitude hold reference); inside a cloud or dense fog visibility drops to near zero; rain adds drag and weight of water; flying in rain above the profile's **IP rating** can cause motor/ESC failures (seeded) (S); icing in clouds below 0 °C (C). | M / S / C |
| FR-WX-07 | **Effects on ground drones:** rain, snow and mud change tyre grip and rolling resistance per terrain material (wet asphalt/rock, wet grass, mud, snow depth); water ingress above the IP rating as for aerial (S). | M / S |
| FR-WX-08 | **Effects on the battery:** ambient temperature sets battery temperature (FR-PHY-07); cold weather shortens flight/drive time. | M |
| FR-WX-09 | **Rendering:** cloud layers with lighting from sun and moon (noise-based layers M, ray-marched volumetric clouds S); cloud shadows on the terrain (S); overcast skies diffuse sunlight (soft/no shadows); rain streaks and snowflakes around the camera; fog and visibility applied to aerial perspective; wet surfaces look darker and shinier (S); raindrops on the FPV lens (S); lightning flashes (C). At night clouds hide the moon and stars. | M / S / C |
| FR-WX-10 | **Briefing and HUD:** the briefing shows a weather summary (e.g. *Wind 270° level 4 (24 km/h, gusts 34) · broken clouds at 900 m · light rain · visibility 6 km · 12 °C · QNH 1009*), optionally in METAR style (S). The timeline is not revealed unless the mission says so (`forecast: true`). | M / S |
| FR-WX-11 | **Mission rules on weather:** e.g. fail when flying into cloud (`noCloudEntry`), or require landing before rain arrives. | S |
| FR-WX-12 | **Free mode:** the Environment tab offers the same generator (preset, parameters, seed with *Randomise*, timeline on/off). | M |
| FR-WX-13 | Real or historical weather from online weather services. | C |

**Weather in the mission JSON** (comments only explain the fields; real mission files are strict JSON):
```jsonc
"environment": {
  "utc": "2026-10-03T16:20:00Z",
  "timeZone": "Europe/Madrid",
  "lightPollution": 4,
  "weather": {
    "seed": 90210,                        // same seed + settings ⇒ same weather
    "preset": "broken-clouds",
    "temperatureC": 12, "humidityPct": 80, "pressureHpa": 1009,
    "wind": { "level": 4, "fromDeg": 270 },
    "clouds": [ { "type": "stratocumulus", "coverOktas": 6, "baseM": 900, "topM": 1500 } ],
    "precipitation": { "type": "none" },
    "visibilityM": 9000,
    "timeline": [                         // rain and stronger wind arrive after 4 min
      { "atS": 240, "transitionS": 120, "preset": "light-rain", "wind": { "level": 6 } }
    ]
  }
}
```

### 4.3 Input (`core/input`)

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-IN-01 | **RC transmitter / gamepad** via SDL3 joystick API (USB HID, e.g. EdgeTX/OpenTX radios in joystick mode, Xbox/PS controllers). Hot-plug supported. | M |
| FR-IN-02 | Calibration wizard: stick centres/endpoints, axis inversion, channel order (AETR/TAER…), stick mode 1/2/3/4. | M |
| FR-IN-03 | Switch mapping: arm, flight/drive mode, beeper, RTH, payload release, camera trigger. | M |
| FR-IN-03a | Separate control mappings per vehicle type: aerial (throttle/yaw/pitch/roll) and ground (throttle forward/reverse + steering; gamepad triggers supported). | M |
| FR-IN-04 | Per-axis deadzone and filtering; profiles persisted in DB. | M |
| FR-IN-05 | Keyboard/mouse control for testing and free flight/drive. Missions may disallow it (mission flag `allowKeyboard`). | M |
| FR-IN-06 | Camera control with mouse (non-FPV cameras); `Esc` releases mouse. | M |

Default keyboard mapping (Mode 2): `W/S` throttle, `A/D` yaw, arrows pitch/roll,
`Space` arm/disarm, `1–4` flight modes, `R` reset, `P` pause, `C` cycle camera.
Ground drone: `W/S` forward/reverse, `A/D` steer, `Space` brake (held), `Enter` arm/disarm
(also for aerial drones), other keys as above.

### 4.4 Graphics Engine (`core/graphics`)

**Responsibility:** Render the world (terrain, sky, items, drone, mission markers,
debug overlays, HUD) using OpenGL 4.5 Core.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-GFX-01 | Require an OpenGL **4.5 Core Profile** context; on failure, show a clear error dialog with the detected version and exit cleanly. | M |
| FR-GFX-02 | Use DSA functions (`glCreateBuffers`, `glNamedBufferStorage`, `glCreateVertexArrays`, …). | M |
| FR-GFX-03 | RAII wrappers for GL objects (buffer, VAO, texture, shader program, framebuffer); move-only. | M |
| FR-GFX-04 | Shader management: load GLSL from `assets/shaders`, report compile/link errors with file + line, hot-reload in Debug builds. | M / S (hot-reload) |
| FR-GFX-05 | Cameras (both vehicle types): **FPV** (camera on drone, configurable uptilt and lens FOV), **Line-of-sight** (fixed pilot position on the ground — realistic LOS flying), **Chase**, **Orbit**, **Free-fly** (debug, disabled in missions). | M |
| FR-GFX-06 | **Large-world rendering:** reversed-Z depth buffer, camera-relative rendering (positions relative to the camera computed in `double` before converting to `float`), far plane ≥ 30 km for real-terrain scenes. | M |
| FR-GFX-07 | Terrain: chunked meshes, frustum culling, distance LOD, height/slope texture splatting, normal-based lighting; **orthophoto** albedo from orthophoto files (§4.5.5) or, later, an online imagery provider (§4.5.3), streamed by level of detail with bounded GPU memory. | M |
| FR-GFX-08 | Sky: physically based atmospheric scattering sky dome lit by the sun **and the moon**, sun disc, moon disc; positions from `core/astro` (§4.7.1). Correct **day, twilight (civil/nautical/astronomical) and night** transitions. | M |
| FR-GFX-08a | **Moon:** textured disc with the correct **phase**, illuminated fraction and terminator orientation (bright limb pointing at the sun), size and brightness; earthshine on the dark part (S). | M / S |
| FR-GFX-08b | **Night sky:** stars from a bright-star catalogue (≈ 9 000 stars to magnitude 6.5) placed by sidereal time, with brightness and colour from magnitude and colour index; fade in/out with sky brightness. Bright planets (Venus, Mars, Jupiter, Saturn) (S). Milky Way (C). Light-pollution level (Bortle 1–9) that hides faint stars (S). | M / S / C |
| FR-GFX-08c | **Lighting:** sun and moon are directional lights with physically based intensity (full sun ~100 000 lux, full moon ~0.1–0.3 lux, moonless night ~0.001 lux); the moon casts shadows when it is the dominant light. Rendering is **HDR** with exposure control (auto and manual) and tone mapping so both noon and moonlit scenes look right. | M |
| FR-GFX-08d | **Vehicle and item lights:** aerial drones have navigation/anti-collision LEDs and an optional spotlight; ground drones have headlights and tail lights; items can have lights (landing-pad lights, beacons, windsock light, street lights). Switchable from an RC switch/key; lights illuminate the scene (local lights with shadows for spotlights/headlights). | M (vehicle lights) / S (item lights) |
| FR-GFX-08e | **FPV camera at night:** low-light noise and reduced dynamic range; optional night-vision/IR camera mode (S); thermal (C). | S / C |
| FR-GFX-09 | Aerial-perspective fog matched to the sky and to the weather's visibility and fog (including dim, moonlit fog at night). | M |
| FR-GFX-10 | Directional-light cascaded shadow maps (drone shadow on ground is an important depth cue). | M |
| FR-GFX-11 | Instanced rendering for repeated items (trees, rocks, gates, buildings). | S |
| FR-GFX-12 | Mesh loading for drone/items (glTF 2.0 preferred, OBJ fallback); spinning props with motion blur disc. | M |
| FR-GFX-13 | Mission visuals: waypoint beacons, landing-pad markers, zone volumes (no-fly, drop, survey area) as translucent shapes, next-objective direction arrow; each toggleable per mission ("realistic" missions may hide aids). | M |
| FR-GFX-14a | **Position panel, always visible on the main screen** (FPV, chase and LOS views, free flight and missions): drone **UTM coordinates** — zone + latitude band, easting, northing in metres (e.g. `30T 440 123.4 E  4 474 567.8 N`) — and drone **elevation** AMSL, plus AGL height (aerial only; for ground drones the elevation is that of the drone on the terrain). Updated ≥ 10 Hz from ground-truth state; GPS-estimated values shown separately in the OSD. Can be moved but not hidden; also mirrored in the status bar. Option to additionally show lat/lon (DD or DMS). | M |
| FR-GFX-14 | HUD/OSD: altitude AGL + AMSL, horizontal/vertical speed, attitude, throttle, flight mode, armed state, battery voltage/% /current, GPS sats/HDOP, home distance and bearing, **wind level + direction arrow relative to the drone heading** (missions may hide it), **clock (local time and UTC)**, lights on/off indicator, **flight-rule indicators** (AGL turns amber at 100 m and red at 120 m; distance to pilot and a line-of-sight indicator showing *visible / hidden*), flight timer, warnings, mission objective panel. | M |
| FR-GFX-14b | **Compass, always visible on the main screen** (FPV, chase, LOS and free camera): a north-up rose with the **geographic (true) north** at the top and the cardinal points **N, E, S, W** (N in red), a **magnetic-north** marker (blue, "MN") turned by the local declination, and an arrow with the **drone's heading**; in the free camera a dashed line also shows the camera's view direction. Read-outs: heading true and magnetic (`HDG 123°T  121°M`) and declination (`DECL 1.2°E`). The declination comes from the World Magnetic Model (FR-GEO-05) at the drone's position and the simulation date; the read-out turns amber outside the model's validity period or in a weak horizontal field (< 6000 nT) and shows `MAG UNRELIABLE` in red near the magnetic poles (< 2000 nT, the WMM blackout zone). | M |
| FR-GFX-15 | Viewport resize, HiDPI, MSAA (0/2/4/8×), V-Sync on/off. | M |
| FR-GFX-16 | GPU debug output (`GL_KHR_debug`) routed to the logger in Debug builds. | M |
| FR-GFX-17 | Debug overlays: wireframe, chunk bounds, collision volumes, force/velocity vectors, wind vector; frame statistics (FPS, frame time, draw calls, triangles). | S |
| FR-GFX-20 | Ground drone visuals: rotating wheels/tracks, steering angles, suspension travel; tyre tracks and dust on soft ground (C). HUD for ground drones: speed, heading, pitch/roll with **rollover warning**, slope under the drone, wheel slip indicator, battery, drive mode. | M / C (tracks, dust) |
| FR-GFX-19 | **Data attribution** text of every visible provider in a viewport corner (FR-TER-O13). | M |
| FR-GFX-18 | FPV analog-video effects (noise, breakup at range/behind terrain). | C |

**Render pipeline (per frame):**
1. Acquire latest `WorldSnapshot`; interpolate between the last two ticks.
2. Update camera; compute camera-relative transforms.
3. Upload finished terrain chunks from the worker queue (time-budgeted per frame).
4. Shadow cascades.
5. Opaque: terrain → items → drone.
6. Sky and clouds (depth at far plane), then aerial-perspective fog.
7. Translucent mission volumes and markers; precipitation particles.
8. Debug overlays → HUD.

### 4.5 Terrain Generator (`core/terrain`)

**Responsibility:** Provide terrain from a configurable **height source** —
**procedural (random, seeded)** or **online DEM (web API)**, with optional online
orthophotos — as streamed chunk meshes for
rendering and as height/normal queries for physics and placement.

#### 4.5.1 Common requirements

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-TER-01 | Abstract `HeightSource` interface; the rest of the module (chunking, LOD, meshing, queries) is source-agnostic. | M |
| FR-TER-02 | Three source types: **Procedural** (§4.5.2), **Online DEM** from web APIs (§4.5.3) and **DEM file** referenced by the mission JSON (§4.5.4). A **Composite** source (real DEM + procedural micro-detail) is optional. | M / S (composite) |
| FR-TER-03 | **Deterministic:** same source + parameters (seed; provider + identical downloaded data, FR-TER-O14; or identical DEM file, FR-TER-F09) ⇒ identical heights on every run. | M |
| FR-TER-04 | Chunked world: chunk size configurable (default 256 m × 256 m, 129 × 129 vertices at LOD 0); chunks generated around the camera within the view radius; evicted beyond it (LRU cache). | M |
| FR-TER-04a | **Optional terrain dimensions:** width × length (metres), centred on the geo origin, may be set **by the user** (Terrain tab / preset) **or by a mission**; a mission value overrides the preset. When **omitted**, procedural terrain is **unbounded** (streamed endlessly) online DEM terrain streams around the drone within the **provider's coverage**, and DEM-file terrain uses the **full file extent**. When set, the world is clipped to that rectangle and the soft geofence / edge handling of FR-TER-O08 applies to its border. Valid range 256 m – 200 km per side. | M |
| FR-TER-05 | Asynchronous generation on the worker pool; never stalls the render or simulation thread. Physics queries for not-yet-built chunks are answered directly from the height source. | M |
| FR-TER-06 | LOD: ≥ 5 levels with crack-free stitching between neighbours (skirts or index stitching). | M |
| FR-TER-07 | Query API: `height(x, z)`, `normal(x, z)`, `heightAMSL(x, z)`; interpolated on the same two triangles per grid cell as the LOD-0 mesh (bilinear interpolation would differ from the rendered surface by more than 1 cm on rough ground); must match the rendered LOD-0 surface within 1 cm. | M |
| FR-TER-08 | Material classification per vertex (grass / rock / sand / snow / water) from height, slope and optional masks. | M |
| FR-TER-09 | Every terrain has a **geographic origin** (lat, lon, alt). For online DEM it is configured by the user or mission (default: centre of the requested area); for a DEM file it defaults to the centre of the file (or of the requested sub-area); for procedural terrain it is user-configurable (default 0°, 0°, 0 m), so GPS, sun position and lat/lon mission coordinates always work. | M |
| FR-TER-10 | Terrain presets (named source configurations) saved/loaded via storage. | M |

#### 4.5.2 Procedural (random) source

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-TER-P01 | Fractal noise (Simplex/Perlin fBm) with parameters: seed, octaves, frequency, lacunarity, persistence, amplitude, base height. | M |
| FR-TER-P02 | "Random" button generates a new 64-bit seed; the seed is always shown and stored so any random terrain can be reproduced. | M |
| FR-TER-P03 | Feature passes: ridged noise (mountains), domain warping, erosion approximation, flat zones (landing areas / mission sites), water level. | M (ridged, flat zones, water) / S (warp, erosion) |
| FR-TER-P04 | Extent follows FR-TER-04a: unbounded streaming when no dimensions are given, otherwise a bounded rectangle; terrain edge treatment (cliff, fade to base height, water) configurable. | M |
| FR-TER-P05 | Procedural scatter of trees/rocks consistent with seed and materials (feeds the Items Manager as instanced static obstacles). | S |

#### 4.5.3 Online DEM source (web APIs)

Online elevation (and, later, online orthophotos) come from **web APIs configured by URL**. Network
access, caching and credentials are handled by `core/net` (§4.6); this section covers what
is requested and how payloads become terrain. Elevation files are covered in §4.5.4 and
orthophoto files in §4.5.5.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-TER-O01 | Elevation protocols: **REST bounding-box endpoints returning GeoTIFF** — first and initially only provider: **OpenTopography** (Global DEM API and USGS 3DEP API) (M). XYZ/TMS tiles (Terrarium, Terrain-RGB, GeoTIFF), OGC WCS, OGC API – Coverages and COG range requests for later providers (S). | M / S |
| FR-TER-O02 | Provider configuration: URL template with placeholders `{dataset} {south} {north} {west} {east} {bbox} {width} {height} {crs} {format} {apiKey}` (tile providers later add `{z} {x} {y}`), protocol, dataset list, encoding, native resolution, CRS, vertical datum, NoData value, coverage area, **max area per request**, **daily call limit**, attribution/citation, licence, cache policy. Built-in presets (`assets/providers/`) plus user-defined providers. | M |
| FR-TER-O03 | Payloads are decoded **in memory**: GeoTIFF and images through GDAL (`/vsimem/`), Terrarium/Terrain-RGB height decoding in project code. GDAL's own network drivers are disabled. | M |
| FR-TER-O04 | CRS handling: Web-Mercator tiles (EPSG:3857) and projected/geographic coverages are reprojected into the scene's local ENU frame anchored at the scene origin. Band scale/offset and vertical units (m / ft) applied. | M |
| FR-TER-O05 | Resolution: bounding-box providers deliver the dataset's native resolution (e.g. 30 m for COP30); coarser LODs are made locally by downsampling. LOD-0 vertex spacing follows the data, clamped to [1 m, 30 m]. Optional coarse dataset (e.g. COP90 / SRTM15+) for distant background terrain (S). Resampling bilinear (M), bicubic (S). | M |
| FR-TER-O06 | **Loading:** bounding-box providers load in **blocks** (FR-TER-O16); tile providers (later) load coarse-first and refine. Physics uses the finest data already available locally and never waits for the network. | M |
| FR-TER-O07 | NoData voids and failed tiles are filled from the next coarser level or by interpolation; persistent gaps are reported in the UI. | M |
| FR-TER-O08 | Extent: with dimensions (FR-TER-04a) a bounded rectangle, otherwise streaming around the drone within provider coverage. Outside the extent or coverage: *clamp edge*, *fade to base height* or *procedural fill*; a soft geofence warns before the edge. A requested extent outside provider coverage is rejected with a clear message. | M |
| FR-TER-O09 | **Area pre-download:** "Download area" for a rectangle + max resolution with size estimate, progress and cancel. Missions on online terrain require their area **fully cached before PreFlight**, so a scored attempt never depends on the network. | M |
| FR-TER-O10 | Online orthophoto providers (after the file-based orthophotos of §4.5.5): **WMTS** and **XYZ/TMS** (S), **WMS GetMap** (C); JPEG/PNG/WebP; reprojected and draped like orthophoto files. Only Web-Mercator tile grids (EPSG:3857, the `GoogleMapsCompatible` matrix set) are supported. Imagery is streamed tile by tile while flying and never downloaded by area. Presets: IGN PNOA (Spain, WMTS), swisstopo SWISSIMAGE and USGS The National Map imagery. | S / C |
| FR-TER-O11 | Vertical exaggeration factor (default 1.0; missions force 1.0). | S |
| FR-TER-O12 | Metadata view: provider, protocol, CRS, coverage, resolution, cached area and size, min/max elevation of the loaded area, attribution. | M |
| FR-TER-O13 | **Attribution:** the attribution required by each provider whose data is visible is shown in the viewport and in debrief screenshots. | M |
| FR-TER-O14 | Data versioning: every cached tile stores ETag / Last-Modified and a content hash; a mission attempt records the hash of the terrain data used, so scores are compared only on identical data. | M |
| FR-TER-O16 | **Block fetching for bounding-box APIs:** API calls are the scarce resource (OpenTopography free keys: 50 calls / 24 h), so data is fetched in as **few, large requests** as possible: with a set extent, one request covers the whole extent plus a margin (split only if it exceeds the provider's area or response-size limit); without an extent, the world is divided into large blocks (default 20 km × 20 km) fetched ahead of the drone. | M |
| FR-TER-O17 | **Quota awareness:** each provider has a daily call budget. OpenTopography is used with **non-academic keys only: 50 calls / 24 h** (custom value possible for Plus/Enterprise keys). Calls are counted over a rolling 24 h in the DB. Before a download the UI shows *"uses N of your M remaining calls today"*; when the budget is used up the app switches that provider to cache-only with a clear message; quota errors are never retried automatically. | M |
| FR-TER-O18 | **Vertical datum:** orthometric datasets are preferred and default (COP30 – EGM2008, SRTM/NASADEM – EGM96); ellipsoidal variants are opt-in. The datum is shown in the metadata and used for the AMSL elevation on the main screen. | M |
| FR-TER-O15 | **Validation:** every payload is checked (content type, dimensions, data type, value range, decompressed-size limit) before use; malformed or unexpected responses become `std::expected` errors and never crash. | M |

#### 4.5.4 Elevation files: rasters (COG, GeoTIFF, …) and LAS/LAZ (defined in the mission JSON)

A mission can name **elevation files** that are loaded from disk instead of procedural or
online terrain. Two kinds of file are supported:

- **Elevation rasters** — **COG** (Cloud-Optimized GeoTIFF, the recommended format), plain
  GeoTIFF and the other raster formats listed below, read directly.
- **LAS / LAZ** — lidar point clouds, turned into an elevation raster (DTM or DSM) on first
  load and cached as a COG.

The files travel with the mission (mission folder) or live in the user's geodata library.

**Common requirements (rasters and point clouds)**

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-TER-F01 | **Formats.** *Rasters* (read with GDAL): **COG** and **GeoTIFF** (M); **SRTM `.hgt`**, **ESRI ASCII Grid (`.asc`)**, **ERDAS Imagine (`.img`)** (S). *Point clouds* (read with PDAL): **LAS 1.0–1.4 and LAZ**, all point formats 0–10 (M). Only these GDAL drivers and PDAL readers are enabled for files. Non-COG rasters are fully supported; for large files the metadata view suggests converting to COG (`gdal_translate -of COG`) for faster loading. | M / S |
| FR-TER-F02 | **Declared in the mission JSON** under `terrain` with `"source": "file"` and a list of `elevationFiles` (examples below): path, optional `sha256` per file, attribution, vertical datum, optional origin and extent; LAS/LAZ add rasterization settings (FR-TER-L03/L04). | M |
| FR-TER-F03 | **Where files may be:** paths are **relative to the mission file's folder** (the mission is a folder containing `*.mission.json` plus its `dem/` and `ortho/` files), or `library:<name>` for the user's shared **geodata library** `$HOME/.drone_simulation/library/` (for large elevation or orthophoto files used by several missions). Absolute paths, `..` segments and symlinks leading outside these folders are **rejected**. | M |
| FR-TER-F04 | **Several files (tiles):** `elevationFiles` may list many tiles (lidar is usually delivered as 1 km² tiles); they are mosaicked into one seamless surface (GDAL VRT for rasters, merged point sets for LAS/LAZ). All files of one terrain must be of the same kind (raster or point cloud) and the same CRS; rasters of different formats may be mixed. | M |
| FR-TER-F05 | **Georeferencing:** CRS from the raster's georeferencing (GeoTIFF keys, `.prj`, etc.; `.hgt` is implicitly EPSG:4326) or the LAS/LAZ header (GeoTIFF or WKT VLRs); projected (e.g. UTM) and geographic CRSs supported; heights reprojected into the scene's local ENU frame. If a file has no CRS, the mission must give `crs` (e.g. `"EPSG:25830"`), otherwise it is rejected. | M |
| FR-TER-F06 | **Vertical units and datum:** metres or feet converted; the datum (e.g. EGM2008, a national geoid, or ellipsoidal) comes from the file or the mission's `verticalDatum` and is used for the AMSL elevation on the main screen. | M |
| FR-TER-F07 | **Extent:** the union of all files, or the mission's `extent` sub-area (rejected with a clear message if not covered, unless *procedural fill* is chosen). Edge handling as FR-TER-O08. | M |
| FR-TER-F08 | **Validation:** missing/unreadable file, unsupported format, mixed formats or CRSs, missing CRS, corrupt data, raster too large (default 4 GB / 40 000 × 40 000 px) or point cloud too large (default 2 billion points / 20 GB per terrain) → `std::expected` error with the mission file and JSON path in the message; never a crash. Files are **untrusted input**. | M |
| FR-TER-F09 | **Integrity:** if the mission gives `sha256` for a file, it must match or the mission is listed as invalid; the combined hash of all input files plus rasterization settings is recorded as the attempt's `terrain_data_hash`. | M |
| FR-TER-F10 | **Attribution:** the mission's `attribution` text (dataset licence/citation) is shown like provider attribution (FR-TER-O13). | M |
| FR-TER-F11 | **Free mode:** the Terrain tab can also open raster or LAS/LAZ files directly (same rules and validation). | S |

**Raster specifics (COG, GeoTIFF, `.hgt`, `.asc`, `.img`)**

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-TER-C01 | Single-band elevation (or a `band` chosen in the mission); Int16, UInt16, Int32, Float32, Float64; scale/offset applied; NoData filled by interpolation for small voids and a fallback height for large ones. | M |
| FR-TER-C02 | **Partial reads:** chunks read only the part of the file they need (COG/tiled GeoTIFF tiles, or windows of other formats); coarse LODs use internal overviews when present (otherwise built in memory or, for large files, a COG copy is cached like FR-TER-L05); memory bounded (default 256 MB). | M |

**LAS / LAZ specifics**

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-TER-L01 | **Reading:** LAS 1.0–1.4 and LAZ via PDAL, streamed in chunks (never the whole cloud in memory); points flagged *withheld* and noise classes (7, 18) are dropped. COPC (Cloud-Optimized Point Cloud) files read as LAZ (S: using COPC's spatial index for faster partial reads). | M / S |
| FR-TER-L02 | **Surface model — v1.0 is DTM only:** ground points (class 2, plus water class 9 if present) give the bare-earth surface. The mission field `surface` exists for forward compatibility; in v1.0 only `"dtm"` (the default) is accepted. A DSM option (highest return per cell, including buildings and trees; aerial missions only, since a rover can't drive over tree tops) is planned after v1.0. | M (DTM) / C (DSM) |
| FR-TER-L03 | **Rasterization:** grid resolution from `resolutionM` (default: automatic from point density, typically 0.5–2 m; minimum 0.25 m); interpolation **TIN** (Delaunay triangulation, default for DTM), IDW or max (DSM); holes (under buildings, water, no returns) filled by TIN/IDW with a maximum hole size, larger gaps marked NoData and filled as in FR-TER-C01. | M |
| FR-TER-L04 | **Unclassified clouds:** if no ground-classified points exist, DTM generation either fails with a clear message, or — if the mission sets `"classifyGround": true` — runs PDAL's ground filter (SMRF) first. | M (message) / S (classification) |
| FR-TER-L05 | **Caching:** the result is written once as a **COG** (with overviews) to `$HOME/.drone_simulation/cache/dem/<hash>.tif`, keyed by the input files' hashes and the rasterization settings; later loads use the cached COG directly. The cache entry is kept while a mission uses it (counted in the cache size limit, FR-NET-05). | M |
| FR-TER-L06 | **Progress:** first-time rasterization runs on the worker pool with progress and cancel in the briefing / Terrain tab; the mission can't start until it is finished. | M |
| FR-TER-L07 | **Lidar features as obstacles** (after v1.0): building (class 6) and high-vegetation (class 5) points converted into collidable obstacles (building footprints with heights, tree cylinders) on top of the DTM. In v1.0, obstacles on lidar terrain are placed as mission items. | C |
| FR-TER-L08 | **Point-cloud view** (after v1.0): render the raw points as an overlay (by elevation, classification or intensity) for inspection. | C |

**Terrain from raster files in the mission JSON:**
```json
"terrain": {
  "source": "file",
  "format": "raster",
  "elevationFiles": [
    { "path": "dem/valley_utm30n.tif", "sha256": "5f0c…e9a1" }
  ],
  "verticalDatum": "EGM2008",
  "attribution": "Contains modified Copernicus DEM data 2021, © ESA",
  "origin": { "geo": [40.4170, -3.7036, 0], "altRef": "AGL" },
  "extent": { "widthM": 4000, "lengthM": 4000 }
}
```

**Terrain from LAZ lidar tiles in the mission JSON:**
```json
"terrain": {
  "source": "file",
  "format": "laz",
  "elevationFiles": [
    { "path": "dem/tile_440_4474.laz", "sha256": "a1b2…" },
    { "path": "dem/tile_441_4474.laz", "sha256": "c3d4…" },
    { "path": "library:tile_440_4475.laz" }
  ],
  "crs": "EPSG:25830",
  "verticalDatum": "EGM08-REDNAP",
  "surface": "dtm",
  "resolutionM": 1.0,
  "interpolation": "tin",
  "classifyGround": false,
  "attribution": "LiDAR data: <provider>, CC BY 4.0"
}
```

**Mission folder layout:**
```
valley-survey/
├── valley-survey.mission.json
├── dem/
│   ├── tile_440_4474.laz
│   └── tile_441_4474.laz
└── ortho/
    ├── ortho_440_4474.tif
    └── ortho_441_4474.tif
```

#### 4.5.5 Orthophoto files (defined in the mission JSON)

A mission can drape **orthophotos** (aerial/satellite images) over its real terrain. They
are loaded from image files listed in the mission JSON, next to the elevation files.
Orthophotos are visual only: they don't change heights, physics or scoring.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-TER-I01 | **Formats** (read with GDAL): **COG** and **GeoTIFF**, 8-bit RGB or RGBA (M); **JPEG 2000** (`.jp2`) (S); **PNG / JPEG with a world file** (`.pgw`/`.jgw`) plus `crs` in the mission (S); 16-bit imagery scaled to 8-bit with mission-defined min/max (S). Multi-band images use `bands` (e.g. `[1, 2, 3]` for RGB from a 4-band file). | M / S |
| FR-TER-I02 | **Declared in the mission JSON** under `terrain.orthophotoFiles` (list of `{ path, sha256 }`), with optional `crs` (if the files have none), `attribution`, and colour adjustments (`brightness`, `contrast`, `gamma`) (S). Same path rules as elevation files (FR-TER-F03): mission folder (`ortho/`) or `library:`. | M |
| FR-TER-I03 | **Only on georeferenced terrain:** orthophotos can be used with file or online elevation, not with procedural terrain (the validator rejects it). | M |
| FR-TER-I04 | **Several tiles** are mosaicked seamlessly (GDAL VRT); NoData/alpha areas and areas outside the orthophoto coverage fall back to the slope/height materials, with a soft blend at the edge. | M |
| FR-TER-I05 | **Reprojection** into the scene's local ENU frame, matching the elevation exactly (misalignment ≤ 1 orthophoto pixel). | M |
| FR-TER-I06 | **Streaming:** orthophotos are often 0.1–0.5 m per pixel and many GB; they are streamed to the GPU by level of detail around the camera (clipmap / virtual texture) from the file's overviews, within a GPU memory budget (default 1 GB). Files without overviews, or in a different CRS, are converted once to a cached COG with overviews in `$HOME/.drone_simulation/cache/ortho/` (with progress, like FR-TER-L05/L06). | M |
| FR-TER-I07 | **Lighting:** orthophotos are treated as sRGB albedo and lit by the sun, moon and weather like other terrain; their baked-in shadows are left as-is (shadow reduction is C). | M / C |
| FR-TER-I08 | **Validation and limits:** missing/unreadable/corrupt files, unsupported format or band layout, missing CRS, mixed CRSs, too large (default 50 GB per mission / 200 000 × 200 000 px) → clear error with the JSON path; never a crash. Files are untrusted input. A broken orthophoto only disables the imagery (with a warning) — the mission still loads. | M |
| FR-TER-I09 | **Integrity and attribution:** `sha256` checked when given; `attribution` shown like provider attribution (FR-TER-O13). Orthophoto hashes are **not** part of `terrain_data_hash`, since they don't affect scoring. | M |
| FR-TER-I10 | **Free mode:** the Terrain tab can add orthophoto files over a file or online elevation source. | S |
| FR-TER-I11 | Land-cover / water-mask rasters from files (to drive materials, ground friction and water). | C |

**Orthophotos in the mission JSON** (added to a file- or online-based `terrain`):
```json
"terrain": {
  "source": "file",
  "format": "raster",
  "elevationFiles": [ { "path": "dem/valley_utm30n.tif" } ],
  "orthophotoFiles": [
    { "path": "ortho/ortho_440_4474.tif", "sha256": "9e1f…" },
    { "path": "library:ortho_441_4474.tif" }
  ],
  "orthophotoAttribution": "Orthophoto: <provider>, CC BY 4.0"
}
```

**Key interfaces (illustrative):**
```cpp
struct GeoOrigin { double latDeg, lonDeg, altM; };

class HeightSource {
public:
    virtual ~HeightSource() = default;
    [[nodiscard]] virtual float  sample(double east, double north) const = 0; // metres AMSL
    [[nodiscard]] virtual Bounds2D extent() const = 0;                        // local ENU
    [[nodiscard]] virtual GeoOrigin origin() const = 0;
    [[nodiscard]] virtual HeightGrid sampleGrid(ChunkCoord, int lod) const = 0; // thread-safe
};

class ProceduralSource final : public HeightSource { /* NoiseParams, seed */ };

class OnlineDemSource final : public HeightSource {
public:
    static std::expected<std::unique_ptr<OnlineDemSource>, TerrainError>
        create(const ProviderConfig& elevation, net::DataClient& client, const OnlineOptions&);
    [[nodiscard]] const SourceMetadata& metadata() const;
    net::DownloadJob downloadArea(const GeoRect& area, double maxResolutionM);
};

class RasterSource final : public HeightSource {  // one or more raster tiles (VRT mosaic)
public:
    static std::expected<std::unique_ptr<RasterSource>, TerrainError>
        open(std::span<const std::filesystem::path> files, const RasterOptions&);  // paths resolved + checked
    [[nodiscard]] const SourceMetadata& metadata() const;
};

class LidarRasterizer {                            // LAS/LAZ → cached COG, on the worker pool
public:
    static std::expected<std::filesystem::path, TerrainError>   // path of the cached COG
        rasterize(std::span<const std::filesystem::path> files, const LidarOptions&,
                  ProgressSink&, std::stop_token);
};

class OrthophotoSource {                       // mosaic of orthophoto files, streamed by LOD
public:
    static std::expected<std::unique_ptr<OrthophotoSource>, TerrainError>
        open(std::span<const std::filesystem::path> files, const OrthoOptions&, const GeoOrigin&);
    [[nodiscard]] std::optional<ImageTile> tile(TileKey) const;   // thread-safe; nullopt ⇒ use materials
};

class TerrainGenerator {   // chunking, LOD, meshing, caching on top of any source
public:
    explicit TerrainGenerator(std::shared_ptr<const HeightSource>);
    [[nodiscard]] float     height(float x, float z) const;
    [[nodiscard]] glm::vec3 normal(float x, float z) const;
    void requestChunksAround(glm::dvec3 cameraPos, float radius);
    std::vector<ChunkMesh> takeFinishedChunks();
};
```

### 4.6 Network & Data Cache (`core/net`)

**Responsibility:** All communication with elevation and imagery APIs: requests,
credentials, rate limits, retries, disk cache and offline operation.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-NET-01 | HTTP(S) client on libcurl (multi interface) on the network thread; asynchronous requests with **priority** (data near the drone and at visible LOD first) and **cancellation** (when a chunk is evicted); per-provider concurrency limit. | M |
| FR-NET-02 | **HTTPS with certificate verification** by default; plain HTTP only for providers explicitly marked insecure by the user (with a warning). Timeouts (connect 10 s, total 30 s), max 5 redirects, max response size (default 64 MB), allowed content types per provider. | M |
| FR-NET-03 | Retries with exponential backoff + jitter for timeouts and 5xx; honour `429` / `Retry-After`; **daily quota errors are never retried** (FR-TER-O17); per-provider rate limit; circuit breaker after repeated failures, shown in the UI. | M |
| FR-NET-04 | **Per-user disk cache** at `$HOME/.drone_simulation/cache/<provider>/…` (mode `0700`), with an index in `cache.db` (separate SQLite file, accessed through the storage service via the `CacheIndex` interface). Deleting the cache never touches user data. | M |
| FR-NET-05 | Cache size limit (default 5 GB, configurable) with LRU eviction; tiles of downloaded areas and mission areas are **pinned** until the user releases them. Honours each provider's policy (`allowCache`, `maxAgeDays`) and HTTP `Cache-Control` / ETag revalidation. | M |
| FR-NET-06 | **Offline mode:** automatic when the network is unreachable, or manual toggle; only cached data is used; the UI shows the offline state and which areas are cached. | M |
| FR-NET-07 | **Credentials:** API keys per provider are kept in the OS keyring (Secret Service on Linux, Credential Manager on Windows) through the app layer and passed to `core/net` in memory; fallback storage in the user DB (file `0600`) with a warning. Keys are sent as header or query parameter per provider config, **never logged**, and redacted from URLs in logs and error messages. | M |
| FR-NET-08 | Proxy support from the environment (`https_proxy`, `no_proxy`) and from Settings. | S |
| FR-NET-09 | Per-provider usage statistics (calls in the last 24 h vs. daily limit, bytes, errors, cache hit rate) shown in Settings. | M |
| FR-NET-10 | Generic `User-Agent: DroneSimulator/<version>`; no personal data, user IDs or telemetry in requests (the requested area is inherently visible to the provider — stated in the UI). | M |
| FR-NET-11 | "Test connection" for a provider: smallest possible request, validated, with latency and errors reported; for quota-limited providers the UI warns that it uses one call. | M |

**First provider: OpenTopography** (<https://portal.opentopography.org/apidocs/>)

| Item | Value |
|------|-------|
| Endpoints | **Global DEM API** and **USGS 3DEP API**: REST, HTTP GET, bounding box (south, north, west, east) + dataset + output format → raster file. Exact paths and parameter names (expected `/API/globaldem?demtype=…` and `/API/usgsdem?datasetName=…`, `outputFormat=GTiff`, `API_Key=…`) **must be verified against the Swagger spec** (the docs page is JavaScript-rendered). |
| Global datasets | **COP30 (default)**, COP90, SRTM GL1, SRTM GL3, NASADEM, AW3D30, EU DTM 30 m, SRTM15+, GEDI L3, GEBCO, Canada MRDEM, Global Ensemble DTM, ANADEM; ellipsoidal variants (SRTM GL1 Ellip, AW3D30_E) opt-in. |
| USGS datasets (USA) | USGS 30 m, USGS 10 m (USGS 1 m is restricted to academic users, so it is not used). |
| Output | GeoTIFF, decoded with GDAL from memory. |
| Authentication | **Free personal API key** from the user's OpenTopography account. "One user, one key": the app never ships a key; each user enters their own (FR-NET-07). |
| Area limit per request | 30 m datasets 450 000 km²; 90 m 4 050 000 km²; USGS 30 m 225 000 km², 10 m 25 000 km², 1 m 250 km². |
| Rate limit | **Non-academic keys only: 50 calls / 24 h** for the Global and USGS APIs (academic keys are not targeted); more with OpenTopography Plus / Enterprise. |
| Imagery | **None** — elevation only. Orthophotos come from files in the mission (§4.5.5) or from an imagery provider (FR-TER-O10). |
| Terms | Data free of copyright restrictions under the OpenTopography API agreement, but each dataset's licence and citation apply (e.g. Copernicus for COP30). The required OpenTopography acknowledgement and dataset citation are shown as attribution (FR-TER-O13). Commercial, for-profit integration of OpenTopography keys requires an Enterprise key (§12.4.1). |
| Point Elevation / Catalog APIs | Not used in v1.0. |

**Later candidates (S)**, endpoints and terms to verify before adding: AWS Open Data Terrain Tiles
(Terrarium, no key), USGS 3DEP ImageServer, national mapping agencies' WCS/WMTS services
with open licences (elevation and orthophotos). Commercial imagery services (e.g. Google,
Bing, Esri World Imagery) are **not** shipped as presets; their terms generally require
separate agreements. Users may configure other providers and are responsible for
complying with their terms.

**Key interface (illustrative):**
```cpp
namespace net {
class DataClient {
public:
    RequestHandle fetch(const ProviderConfig&, const TileKey& or const BBoxRequest&,
                        Priority, std::function<void(std::expected<Payload, NetError>)>);
    void cancel(RequestHandle);
    void setOffline(bool);
    DownloadJob downloadArea(const ProviderConfig&, const GeoRect&, double maxResolutionM);
};
}
```

### 4.7 Geo-referencing, Time & Astronomy (`core/geo`, `core/astro`)

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-GEO-01 | WGS84 geodetic (lat, lon, h) ⇄ ECEF ⇄ local ENU conversions in `double`, accurate to < 1 mm within 50 km of the origin. | M |
| FR-GEO-02 | Mapping between ENU and the engine's Y-up world frame: `x = East`, `y = Up`, `z = −North`. | M |
| FR-GEO-02a | **WGS84 ⇄ UTM** conversion (Krüger series / Karney, accuracy < 1 mm): zone and latitude band always computed from the **drone's actual position** (when the drone crosses a zone border, the display switches to the new zone), including the Norway/Svalbard zone exceptions; easting/northing in metres; southern hemisphere false northing. UPS is out of scope (polar regions show lat/lon only). | M |
| FR-GEO-02b | Web-Mercator tile math (lat/lon ⇄ tile x/y/z, tile bounds) for XYZ/WMTS providers. | M |
| FR-GEO-03 | (Moved) Sun, moon and sky positions are specified in §4.7.1. | — |
| FR-GEO-04 | Distance/bearing utilities (for HUD home arrow, mission distances, GPS display). | M |
| FR-GEO-05 | Geomagnetic main field from the **World Magnetic Model WMM2025** (NOAA NCEI / BGS, spherical harmonics to degree 12 with secular variation; validity 2025.0–2030.0): north/east/down components, declination, inclination, horizontal and total intensity at a geodetic position and decimal year, matching the official WMM2025 test values (0.01°, 0.05 nT). Used by the compass (FR-GFX-14b) and, later, the magnetometer model. Outside the validity period the field is held at the nearest end and flagged. | M |

#### 4.7.1 Time & Astronomy (`core/astro`)

Mission date and time matter: it can be day or night depending on the date, and the
**sun and moon must be in the correct place in the sky** for the place and moment.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-AST-01 | **Time base:** every scene and mission has a UTC date/time (ISO 8601). Conversions UTC → TT using a ΔT table/polynomial (Espenak–Meeus), Julian dates, Greenwich and local sidereal time. Valid range: years 1900–2100. | M |
| FR-AST-02 | **Local time:** optional IANA time zone (e.g. `Europe/Madrid`) for showing local civil time with correct daylight-saving rules, using the C++20 `std::chrono` time-zone database (fallback: Howard Hinnant's `date/tz`, MIT). Without a time zone, UTC and *local solar time* are shown. | M |
| FR-AST-03 | **Observer:** positions are computed for the drone's current latitude, longitude and altitude (not just the scene origin), so they stay correct on large terrains. | M |
| FR-AST-04 | **Sun position:** apparent azimuth/elevation using a high-accuracy algorithm (NREL SPA or Meeus/VSOP87-truncated), with **atmospheric refraction** near the horizon (corrected for temperature and pressure). | M |
| FR-AST-05 | **Moon position:** apparent azimuth/elevation using Meeus' lunar theory (ELP-2000/82 truncated), **including topocentric parallax** (up to ~1°) and refraction; distance (for apparent size). | M |
| FR-AST-06 | **Moon phase:** illuminated fraction, phase angle, waxing/waning, and **position angle of the bright limb**, so the rendered crescent points the right way for the observer. | M |
| FR-AST-07 | **Events:** sunrise, sunset, solar noon, civil/nautical/astronomical twilight, moonrise, moonset, next full/new moon — for the briefing, the Environment tab and mission rules. Handle polar day/night (no rise/set). | M |
| FR-AST-08 | **Stars:** bright-star catalogue (Yale BSC5) at J2000, precessed to the date; rotated by local sidereal time. Planet positions (S). | M / S |
| FR-AST-09 | **Sky brightness:** sun and moon elevations (and moon phase) drive sky luminance and colour, star visibility and the ambient light level used by rendering and by the FPV camera model. | M |
| FR-AST-10 | All astronomy is a pure function of time and observer position ⇒ deterministic (FR-X-05) and cheap: recomputed at ≤ 10 Hz, interpolated in between. | M |
| FR-AST-11 | Solar and lunar eclipses rendered when they occur at the scene's date and place. | C |

**Mission/scene time fields** (comments only explain the fields; real mission files are strict JSON):
```jsonc
"environment": {
  "utc": "2026-08-12T21:40:00Z",         // required: start date and time
  "timeZone": "Europe/Madrid",           // optional, for local-time display
  "lightPollution": 3,                   // optional, Bortle 1–9
  "weather": { "seed": 7, "preset": "clear", "temperatureC": 22,
               "wind": { "level": 1, "fromDeg": 45 } }   // §4.2.3
}
```

### 4.8 Items Manager (`core/items`)

**Responsibility:** Own all world entities other than terrain — create, destroy,
query, select, transform and serialise them.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-ITM-01 | Entity registry with stable unique IDs (64-bit, generational to detect stale handles). | M |
| FR-ITM-02 | Built-in item types: **Drone** (aerial or ground), **Home point / Spawn**, **Obstacle** (box, cylinder, tree, building, house, cow, man, power line, wind turbine), **Gate**, **Waypoint**, **Landing pad**, **Payload**, **Pickup / Drop zone**, **Inspection target**, **Survey area** (polygon), **No-fly zone / Geofence** (polygon prism or cylinder), **Pilot position** (LOS camera and line-of-sight checks; defaults to the spawn/home point). | M |
| FR-ITM-03 | Each item: id, type, name, transform (position, rotation quaternion, scale), optional geo position (lat/lon/alt with altitude reference AGL/AMSL), collision shape, render mesh, enabled/visible flags, type-specific properties. | M |
| FR-ITM-04 | CRUD: add, remove, duplicate, rename, transform, enable/disable. | M |
| FR-ITM-05 | Spatial queries: radius / AABB / ray / point-in-zone; uniform grid or BVH. | M |
| FR-ITM-06 | Mouse picking from the viewport; selected item highlighted. | S |
| FR-ITM-07 | Terrain placement: snap to terrain height; AGL-referenced items follow terrain when the source changes. | M |
| FR-ITM-08 | Trigger detection: gate pass-through (direction-aware), zone enter/exit, waypoint reach — emitted as events consumed by missions. | M |
| FR-ITM-09 | Undo/redo for item edits (command pattern). | S |
| FR-ITM-10 | Change notifications so the GUI list and renderer stay in sync. | M |
| FR-ITM-11 | Serialisation as part of a Scene or Mission. | M |
| FR-ITM-12 | Moving items (vehicles, birds, follow targets) on scripted paths. | C |
| FR-ITM-13 | Ground-drone items: **ramps**, **bridges**, **parking zone** (target box with heading), road/track markers. | S (ramps, bridges) / M (parking zone) |
| FR-ITM-14 | Missions with several vehicles at once (e.g. an aerial and a ground drone cooperating). | C |

### 4.9 Missions (`core/missions`)

**Responsibility:** Load, validate and run missions: track objectives, enforce
constraints, detect failure, score the attempt, and report results.

#### 4.9.1 Mission definition (JSON file)

**Each mission is described by one JSON file**, which is the single source of truth.

- **File:** UTF-8, strict JSON (no comments), extension `.json` (`.mission.json` recommended),
  max 5 MB. In the mission folders every `*.mission.json` is a mission, and so is every other
  `*.json` whose top-level object has `schemaVersion` or `objectives` (a malformed `.json` is
  listed as invalid so its author sees why); other JSON files, `*.schema.json` and names
  starting with `.` are ignored.
- **Self-contained:** the file holds everything needed to play the mission: terrain source
  and area, environment, vehicle, rules, items, objectives, failures and scoring. It refers
  to nothing outside itself except by stable keys of **built-in** resources
  (reference vehicle profiles, data-provider presets) and, for file terrain, its
  **elevation and orthophoto files** inside the mission folder or the user's geodata library (§4.5.4, §4.5.5). A mission
  may embed a full custom vehicle profile instead of referencing one.
- **Schema:** formally defined by a **JSON Schema (draft 2020-12)** shipped as
  `assets/missions/mission.schema.json`. The app validates with that same schema file, and
  mission authors can use it in any editor (autocomplete, validation). Unknown properties
  are errors, except extension fields prefixed with `x-`.
- **Versioning:** `schemaVersion` is required; older versions are migrated on load,
  newer versions are rejected with a clear message.
- **Locations:** built-in missions in `/opt/drone_simulator/share/missions/`; the user's
  missions in `$HOME/.drone_simulation/missions/` (scanned at startup: **every valid mission
  file there is listed automatically**, whatever its name). A mission is either a single
  mission file or a **folder** with the JSON plus its `dem/` files; *Import
  mission* copies the file or the whole folder there.
- **Default mission:** at every start the application makes sure the user's mission folder
  holds the **default mission** *First flight* (`first-flight.mission.json`, shipped in
  `/opt/drone_simulator/share/default-missions/`), so the list of available missions is never
  empty and new users have a ready-made example to fly and to copy (FR-MIS-01b).
- **Units and coordinates:** SI units; positions as `geo` `[lat, lon, alt]` with `altRef`
  (`AGL`/`AMSL`), as `utm` `{zone, easting, northing, alt}`, or as `local` metres from the
  terrain origin in the engine frame (`[x east, y up, z south]`).

**Example — aerial mission on online terrain:**

```json
{
  "schemaVersion": 1,
  "id": "precision-landing-01",
  "title": "Precision landing in crosswind",
  "briefing": "Take off from the yard, fly to the rooftop pad and land within 0.5 m.",
  "difficulty": 2,
  "chapter": "basics",
  "terrain": {
    "source": "online",
    "elevationProvider": "opentopography",
    "dataset": "COP30",
    "imageryProvider": null,
    "origin": { "geo": [46.5100, 7.9800, 0], "altRef": "AGL" },
    "extent": { "widthM": 3000, "lengthM": 2000 }
  },
  "environment": { "utc": "2026-06-21T09:30:00Z", "timeZone": "Europe/Zurich",
                   "weather": { "seed": 1201, "preset": "scattered-clouds", "temperatureC": 18,
                                "wind": { "level": 4, "fromDeg": 270 } } },
  "vehicle":  { "type": "aerial", "profile": "camera-250g", "allowProfileChange": false },
  "spawn":    { "geo": [46.5098, 7.9795, 0], "altRef": "AGL", "headingDeg": 90 },
  "rules":    { "allowKeyboard": false, "showMarkers": true,
                "maxAltitudeAglM": 120, "vlos": { "maxDistanceM": 500, "maxHiddenS": 5 },
                "timeLimitS": 300, "minBatteryPct": 15 },
  "items":    [ { "type": "landingPad", "id": "pad", "geo": [46.5123, 7.9811, 2.0], "altRef": "AGL" } ],
  "objectives": [
    { "id": "takeoff", "type": "reachAltitude", "altitudeAglM": 10 },
    { "id": "land", "type": "land", "target": "pad", "maxErrorM": 0.5,
      "maxTouchdownSpeedMs": 0.7, "after": ["takeoff"] }
  ],
  "failures": [ { "atS": 90, "type": "gpsDrift", "magnitudeM": 3 } ],
  "scoring":  { "stars": [ { "maxTimeS": 240 }, { "maxTimeS": 150, "maxErrorM": 0.3 } ] }
}
```

**Example — ground mission on procedural terrain:**

```json
{
  "schemaVersion": 1,
  "id": "rover-delivery-01",
  "title": "Hillside delivery",
  "briefing": "Carry the sample case up the hill and park in the marked bay.",
  "difficulty": 1,
  "chapter": "ground-basics",
  "terrain":  { "source": "procedural", "seed": 734211, "preset": "rolling-hills",
                "extent": { "widthM": 1500, "lengthM": 1500 } },
  "environment": { "utc": "2026-04-10T15:00:00Z",
                   "weather": { "seed": 55, "preset": "overcast", "temperatureC": 12,
                                "wind": { "level": 2, "fromDeg": 180 },
                                "timeline": [ { "atS": 300, "transitionS": 90, "preset": "light-rain" } ] } },
  "vehicle":  { "type": "ground", "profile": "rover-4wd-3kg" },
  "spawn":    { "local": [0, 0, 0], "headingDeg": 0 },
  "rules":    { "allowKeyboard": true, "timeLimitS": 600, "minBatteryPct": 10,
                "maxStuckS": 20 },
  "items": [
    { "type": "pilot", "id": "pilot", "local": [200, 0, -260] },
    { "type": "pickupZone", "id": "pickup", "local": [40, 0, -15],  "radiusM": 3 },
    { "type": "parkingZone", "id": "bay", "local": [380, 0, -520], "sizeM": [2, 3], "headingDeg": 45 }
  ],
  "objectives": [
    { "id": "load",  "type": "deliver", "pickup": "pickup", "cargoKg": 1.2, "drop": "bay" },
    { "id": "park",  "type": "park", "target": "bay", "maxErrorM": 0.5,
      "maxHeadingErrorDeg": 10, "after": ["load"] }
  ],
  "scoring":  { "stars": [ { "maxTimeS": 480 }, { "maxTimeS": 300, "maxErrorM": 0.25 } ] }
}
```

The full reference of every field, objective, failure and scoring option is in
`docs/MISSIONS.md`, generated from the schema.

#### 4.9.2 Requirements

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-MIS-01 | Missions are loaded **only from JSON files** (§4.9.1): the built-in pack and the user's mission folder. At every start both folders (and their mission sub-folders) are scanned and **every valid mission is added to the list of missions** automatically — `*.mission.json` files and any other `*.json` file that is a mission (top-level `schemaVersion` or `objectives`); no registration or import step is needed. Each file is validated against `mission.schema.json`, then semantically (references between ids, objectives allowed for the vehicle type, positions inside the terrain extent, reachable thresholds). Errors give the file, JSON path and a readable message (e.g. `objectives[1].target: unknown item "pad2"`); an invalid file never crashes the app and is listed as invalid. | M |
| FR-MIS-01a | *Reload missions* rescans the folders; changed files are picked up (file hash). Automatic reload when a file changes on disk (S). | M / S |
| FR-MIS-01b | **Default mission.** At start-up, before the folders are scanned, every mission of `share/default-missions/` is copied into `$HOME/.drone_simulation/missions/` (folder created with mode `0700`, file `0600`) **unless** a file of that name already exists there or a user mission already has that `id`. Existing files are never overwritten (the user's edits are kept); a deleted default mission is installed again at the next start. The default mission is then listed with the user's missions (origin *user*, outside the campaign, so always unlocked). v1.0 ships one: **`first-flight`** — aerial, procedural flat farmland (`flatlands` preset), clear summer morning, light wind, sub-250 g camera drone, keyboard allowed, no time limit; objectives in order: climb to 10 m, reach a beacon 60 m north at 10 m AGL, land on the home pad (≤ 1 m from its centre); stars for finishing in ≤ 120 s and ≤ 60 s. If the folder cannot be written the application still starts, with the built-in missions only. | M |
| FR-MIS-02 | A mission defines its own terrain inline: **procedural** (seed + parameters, optionally starting from a built-in preset), **online** (elevation/imagery provider keys, origin) or **file** (elevation files in the mission folder or geodata library, §4.5.4), optionally with **orthophoto files** (§4.5.5), with optional dimensions (`terrain.extent`, FR-TER-04a). Online terrain must be fully cached before PreFlight (download offered in the briefing). If the provider is unavailable or not configured (e.g. missing API key), the area isn't covered, or the elevation file is missing, invalid or doesn't match its `sha256`, the mission is listed as unavailable with the reason. | M |
| FR-MIS-03 | Item positions given in local metres **or** geographic coordinates (lat/lon + AGL/AMSL), converted via `core/geo`. | M |
| FR-MIS-03a | Every mission declares its **vehicle type** (`aerial` or `ground`); the validator rejects objectives and rules that don't apply to that type. The mission list shows the vehicle type. | M |
| FR-MIS-04 | Objective types: **reachAltitude**, **reachWaypoint** (radius, altitude band, optional hold time, max speed), **hover** (position/altitude tolerance for a duration), **land** (target, max error, max touchdown speed, heading tolerance), **gateCourse** (ordered gates, laps), **deliver** (pickup payload → drop in zone, max drop speed), **inspect** (keep target in FPV camera frustum within distance/angle for N seconds or trigger capture), **survey** (cover a polygon at altitude band, ≥ X % coverage), **returnHome**. | M (all except survey, inspect) / S (survey, inspect) |
| FR-MIS-04a | Objectives per vehicle type. **Aerial:** all of the above. **Ground:** reachWaypoint (2D, no altitude band), gateCourse, deliver (carry cargo from pickup to drop zone), inspect, survey (drive to cover an area), returnHome, and **park** (stop inside a parking zone within position/heading tolerance and max speed). Not available for ground: reachAltitude, hover, land. | M |
| FR-MIS-05 | Objective ordering: sequential, parallel, or dependency graph (`after`); optional objectives with bonus score. | M |
| FR-MIS-06 | Failure conditions: crash; battery below minimum or depleted; geofence / no-fly-zone violation; **flight rules broken** (FR-MIS-06b); time limit exceeded; payload lost; objective-specific failures (e.g. hard landing). Ground drones additionally: **rollover**, **stuck** longer than the mission limit, **water entered**, collision above a speed limit; altitude limits don't apply. | M |
| FR-MIS-06b | **Flight rules enforced by default** in every aerial mission, unless the mission explicitly overrides them: **(1) maximum height 120 m AGL**, measured above the terrain directly below the drone (`rules.maxAltitudeAglM`, default 120); **(2) visual line of sight (VLOS)** between the pilot position and the drone: horizontal distance ≤ `rules.vlos.maxDistanceM` (default 500 m, further limited by the weather's visibility), and the drone may not be hidden behind terrain or items (ray test from the pilot's eye height, 1.7 m) for longer than `rules.vlos.maxHiddenS` (default 5 s). At night, VLOS also requires the vehicle lights to be on. Breaking a rule shows a warning first (at 100 m AGL, at 90 % of the distance, as soon as the drone is hidden) and fails the mission if it lasts longer than `rules.graceS` (default 3 s; the hidden time uses `maxHiddenS`). Ground missions apply VLOS only (no height limit). | M |
| FR-MIS-06c | **Overrides:** a mission may raise or remove the limits (e.g. `"maxAltitudeAglM": null`, `"vlos": { "enabled": false }` for a beyond-line-of-sight scenario). Overrides are listed in the briefing as *Special rules* so the pilot always knows which rules apply. | M |
| FR-MIS-06d | **Free mode:** the same rules are checked and shown as HUD warnings but don't end the flight; enforcement can be switched on in Settings. | M |
| FR-MIS-06a | **Day/night rules:** a mission's `utc` decides whether it is flown by day or night; missions can require vehicle lights on (`requireLightsOn`), forbid flight outside daylight or civil twilight (`daylightOnly`), or end at sunset/sunrise (`endAt: "sunset"`). The briefing shows the local time, sun and moon situation (e.g. *Night — moon 23 % waxing crescent, sets 01:12*), sunrise/sunset, and warns when the mission takes place at night. | M |
| FR-MIS-07 | Scheduled or triggered failure injection (§FR-PHY-15); weather changes come from the weather timeline (§4.2.3). | S |
| FR-MIS-08 | Lifecycle state machine: `Briefing → PreFlight (arming checks) → InProgress → Succeeded / Failed / Aborted → Debrief`. Retry restarts from a clean state. | M |
| FR-MIS-09 | Rules enforced during missions: time scale 1×, free camera disabled, profile locked unless allowed, keyboard allowed only if `allowKeyboard`, terrain exaggeration 1.0, no resets, flight rules per FR-MIS-06b. | M |
| FR-MIS-10 | Scoring: completion, time, precision (landing error, path deviation), battery remaining, smoothness (jerk), penalties; 0–3 stars from mission thresholds. | M |
| FR-MIS-11 | Debrief: per-objective results, score breakdown, failure reason, flight path drawn over a top-down terrain view, comparison with personal best. | M (results) / S (path view) |
| FR-MIS-12 | Campaign: missions grouped into chapters with unlock rules (e.g. ≥ 1 star on previous); progress stored per user in the DB. | M |
| FR-MIS-13 | Built-in pack: **≥ 10 aerial missions** and **≥ 4 ground missions** covering every objective type: at least 3 on **real terrain from DEM files shipped with the mission** (openly licensed data, playable offline), at least 1 on OpenTopography (needs the user's key), and the rest on seeded procedural terrain. | M |
| FR-MIS-14 | Mission editor in the GUI (place items, define objectives/rules, test-fly). It reads and writes the same `.mission.json` files; JSON stays the source of truth. | C |
| FR-MIS-16 | Command-line validation: `drone_simulator --validate-mission <file>` prints errors and exits with a non-zero code, for mission authors and CI. | M |
| FR-MIS-15 | Ghost replay of the personal-best attempt. | C |

**Key interface (illustrative):**
```cpp
class MissionRunner {
public:
    static std::expected<MissionRunner, MissionError> load(const MissionDefinition&, World&);
    void start();
    void update(const WorldSnapshot&, std::span<const SimEvent> events);   // each tick
    [[nodiscard]] MissionState state() const;
    [[nodiscard]] std::span<const ObjectiveStatus> objectives() const;
    [[nodiscard]] std::optional<MissionResult> result() const;
};
```

### 4.10 Storage (`core/storage`)
*Implements the "each user has his own database" requirement.*

"User" means the **operating-system user running the application process**. The app
never sees, lists or opens other users' databases. It has no in-app accounts, login,
user table or user switching: to use a different database, log in as a different OS
user (or use `--db <path>` for testing).

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-DB-01 | Database location: `<home>/.drone_simulation/drone_simulation.db`, where `<home>` is the current process user's home directory (`QDir::homePath()`, i.e. `$HOME` on Linux). Create directory (mode `0700`) and DB on first launch. | M |
| FR-DB-02 | If `$HOME` is unset/unwritable, show an error and run with an in-memory DB (settings not persisted). | M |
| FR-DB-03 | Schema versioning via a `schema_version` table and ordered, idempotent migrations applied at startup inside a transaction. | M |
| FR-DB-04 | Open with `QSqlDatabase::addDatabase("QSQLITE", <connectionName>)`; on open, execute `PRAGMA foreign_keys=ON` and `PRAGMA journal_mode=WAL`. Fail with a clear error if the `QSQLITE` driver is unavailable (`QSqlDatabase::isDriverAvailable`). | M |
| FR-DB-05 | All SQL uses `QSqlQuery::prepare()` + `bindValue()` with named placeholders (no string concatenation). | M |
| FR-DB-06 | Connections are opened/closed by an RAII `DbConnection` that calls `QSqlDatabase::removeDatabase()` on destruction; transactions use an RAII `Transaction` guard (`transaction()` / `commit()`, `rollback()` on scope exit). `QSqlError` is mapped to `std::expected<T, StorageError>` at the API boundary. | M |
| FR-DB-07 | A `QSqlDatabase` connection may only be used from the thread that created it: the storage service owns one connection on a dedicated storage thread (unique connection name per thread). Other threads submit requests to it; the GUI never blocks on long writes (telemetry is batched). | M |
| FR-DB-08 | Backup the DB file before applying a migration (`*.db.bak-v<N>`). | S |
| FR-DB-09 | Built-in missions, reference vehicle profiles (aerial and ground) and data-provider presets are synced into the DB on startup (inserted/updated by stable key, never overwriting user-created entries). | M |
| FR-DB-10 | Export/import scenes as JSON files. Missions are always files (§4.9.1); the DB only stores their index and the user's progress. | S |

**Initial schema (v1):**

The tile cache index lives in a separate `cache.db` (tiles: provider, key, path, size,
etag, last_modified, content_hash, last_access, pinned) so the cache can be wiped independently.

```sql
CREATE TABLE schema_version (version INTEGER NOT NULL);

CREATE TABLE settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL                     -- JSON-encoded value
);

CREATE TABLE vehicle_profiles (
    id           INTEGER PRIMARY KEY,
    profile_key  TEXT NOT NULL UNIQUE,      -- stable key ("camera-250g", "rover-4wd-3kg")
    vehicle_type TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    name         TEXT NOT NULL,
    origin       TEXT NOT NULL CHECK (origin IN ('builtin', 'user')),
    params_json  TEXT NOT NULL,             -- aerial: frame, motors, props, battery, sensors, PIDs, rates
                                            -- ground: chassis, wheels/tracks, suspension, tyres, drivetrain, battery, sensors
    created_at  TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at  TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE data_providers (
    id            INTEGER PRIMARY KEY,
    provider_key  TEXT NOT NULL UNIQUE,
    name          TEXT NOT NULL,
    kind          TEXT NOT NULL CHECK (kind IN ('elevation', 'imagery')),
    protocol      TEXT NOT NULL,             -- xyz, tms, wmts, wms, wcs, rest_bbox, cog…
    url_template  TEXT NOT NULL,             -- never contains the key itself
    params_json   TEXT NOT NULL,             -- encoding, tile size, zooms, CRS, units, NoData, coverage
    attribution   TEXT NOT NULL,
    licence       TEXT,
    requires_key  INTEGER NOT NULL DEFAULT 0,
    allow_cache   INTEGER NOT NULL DEFAULT 1,
    max_age_days  INTEGER,
    rate_limit_rps REAL,
    daily_call_limit INTEGER,                -- e.g. OpenTopography non-academic key: 50
    insecure_http INTEGER NOT NULL DEFAULT 0,
    origin        TEXT NOT NULL CHECK (origin IN ('builtin', 'user'))
);

CREATE TABLE provider_calls (               -- rolling 24 h quota accounting (FR-TER-O17)
    id          INTEGER PRIMARY KEY,
    provider_id INTEGER NOT NULL REFERENCES data_providers(id) ON DELETE CASCADE,
    called_at   TEXT NOT NULL,
    dataset     TEXT,
    west REAL, south REAL, east REAL, north REAL,
    http_status INTEGER,
    bytes       INTEGER
);

CREATE TABLE provider_credentials (          -- fallback only when no OS keyring is available
    provider_id INTEGER PRIMARY KEY REFERENCES data_providers(id) ON DELETE CASCADE,
    api_key     TEXT NOT NULL
);

CREATE TABLE terrain_presets (
    id          INTEGER PRIMARY KEY,
    preset_key  TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    source_type TEXT NOT NULL CHECK (source_type IN ('procedural', 'online', 'file')),
    seed        INTEGER,                    -- procedural only
    elevation_provider_id INTEGER REFERENCES data_providers(id) ON DELETE RESTRICT,
    dem_files   TEXT,                       -- file only (free mode, FR-TER-F11): JSON list of paths + options
    ortho_files TEXT,                       -- optional orthophoto files (free mode, FR-TER-I10)
    dem_sha256  TEXT,
    imagery_provider_id   INTEGER REFERENCES data_providers(id) ON DELETE SET NULL,
    origin_lat  REAL NOT NULL DEFAULT 0,
    origin_lon  REAL NOT NULL DEFAULT 0,
    origin_alt  REAL NOT NULL DEFAULT 0,
    extent_width_m  REAL,                   -- optional: NULL ⇒ unbounded streaming
    extent_length_m REAL,
    params_json TEXT NOT NULL,              -- noise params or online options (resolution, edge mode…)
    CHECK ((source_type = 'procedural' AND seed IS NOT NULL) OR
           (source_type = 'online' AND elevation_provider_id IS NOT NULL) OR
           (source_type = 'file' AND dem_files IS NOT NULL)),
    CHECK ((extent_width_m IS NULL) = (extent_length_m IS NULL))
);

CREATE TABLE downloaded_areas (              -- pinned cache areas (user downloads, missions)
    id            INTEGER PRIMARY KEY,
    provider_id   INTEGER NOT NULL REFERENCES data_providers(id) ON DELETE CASCADE,
    west REAL NOT NULL, south REAL NOT NULL, east REAL NOT NULL, north REAL NOT NULL,
    max_resolution_m REAL NOT NULL,
    mission_id    INTEGER REFERENCES missions(id) ON DELETE CASCADE,
    status        TEXT NOT NULL CHECK (status IN ('pending', 'partial', 'complete')),
    data_hash     TEXT,
    downloaded_at TEXT
);

CREATE TABLE scenes (
    id                INTEGER PRIMARY KEY,
    name              TEXT NOT NULL UNIQUE,
    terrain_preset_id INTEGER REFERENCES terrain_presets(id) ON DELETE SET NULL,
    environment_json  TEXT NOT NULL,        -- UTC date/time, time zone, light pollution, weather generator settings (§4.2.3)
    created_at        TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at        TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE scene_items (
    id              INTEGER PRIMARY KEY,
    scene_id        INTEGER NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,
    type            TEXT NOT NULL,
    name            TEXT NOT NULL,
    pos_x REAL, pos_y REAL, pos_z REAL,
    rot_w REAL, rot_x REAL, rot_y REAL, rot_z REAL,
    scale_x REAL, scale_y REAL, scale_z REAL,
    properties_json TEXT NOT NULL DEFAULT '{}',
    sort_order      INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE missions (                    -- index of mission JSON files (files are the source of truth)
    id                 INTEGER PRIMARY KEY,
    mission_key        TEXT NOT NULL UNIQUE,   -- "id" field of the mission JSON
    title              TEXT NOT NULL,
    vehicle_type       TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    origin             TEXT NOT NULL CHECK (origin IN ('builtin', 'user')),
    chapter            TEXT,
    sort_order         INTEGER NOT NULL DEFAULT 0,
    file_path          TEXT NOT NULL,
    file_sha256        TEXT NOT NULL,          -- detects changed files on rescan
    schema_version     INTEGER NOT NULL,
    status             TEXT NOT NULL CHECK (status IN ('valid', 'invalid', 'missing')),
    validation_errors  TEXT,                   -- JSON array, when invalid
    updated_at         TEXT NOT NULL DEFAULT (datetime('now'))
);                                          -- rows are kept when a file disappears ('missing'), so progress survives

CREATE TABLE input_profiles (
    id           INTEGER PRIMARY KEY,
    device_guid  TEXT NOT NULL,             -- SDL joystick GUID, or 'keyboard'
    vehicle_type TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    name         TEXT NOT NULL,
    mapping_json TEXT NOT NULL,             -- channels, calibration, switches, deadzones
    UNIQUE (device_guid, vehicle_type, name)
);

CREATE TABLE sessions (                    -- one flight or drive
    id               INTEGER PRIMARY KEY,
    scene_id         INTEGER REFERENCES scenes(id) ON DELETE SET NULL,
    vehicle_profile_id INTEGER REFERENCES vehicle_profiles(id) ON DELETE SET NULL,
    vehicle_type     TEXT NOT NULL CHECK (vehicle_type IN ('aerial', 'ground')),
    started_at       TEXT NOT NULL,
    duration_s       REAL,
    max_altitude_agl_m REAL,                -- aerial only
    max_speed_ms     REAL,
    distance_m       REAL,
    energy_used_wh   REAL,
    crashes          INTEGER NOT NULL DEFAULT 0   -- crashes, or rollovers for ground drones
);

CREATE TABLE mission_attempts (
    id             INTEGER PRIMARY KEY,
    mission_id     INTEGER NOT NULL REFERENCES missions(id) ON DELETE CASCADE,
    session_id     INTEGER REFERENCES sessions(id) ON DELETE SET NULL,
    started_at     TEXT NOT NULL,
    outcome        TEXT NOT NULL CHECK (outcome IN ('succeeded', 'failed', 'aborted')),
    failure_reason TEXT,
    score          REAL NOT NULL DEFAULT 0,
    stars          INTEGER NOT NULL DEFAULT 0 CHECK (stars BETWEEN 0 AND 3),
    duration_s     REAL,
    terrain_data_hash TEXT,                 -- FR-TER-O14; NULL for procedural terrain
    results_json   TEXT NOT NULL            -- per-objective results, score breakdown
);

CREATE VIEW mission_progress AS
SELECT m.id AS mission_id,
       COUNT(a.id)                                         AS attempts,
       COALESCE(SUM(a.outcome = 'succeeded'), 0)           AS successes,
       MAX(a.stars)                                        AS best_stars,
       MAX(a.score)                                        AS best_score,
       MIN(CASE WHEN a.outcome = 'succeeded' THEN a.duration_s END) AS best_time_s
FROM missions m LEFT JOIN mission_attempts a ON a.mission_id = m.id
GROUP BY m.id;

CREATE TABLE telemetry_samples (            -- downsampled (default 20 Hz)
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    t_s REAL NOT NULL,
    pos_x REAL, pos_y REAL, pos_z REAL,
    roll REAL, pitch REAL, yaw REAL,
    speed REAL, alt_agl REAL, throttle REAL, steering REAL,
    battery_v REAL, battery_a REAL,
    PRIMARY KEY (session_id, t_s)
);
```

Schema version 1 (`src/core/storage/schema.cpp`) is this DDL with `IF NOT EXISTS` on every
statement, so a partly created database can be completed, plus the indexes below for the
queries that list by owner or time. `successes` uses `COALESCE` so a mission with no attempts
reports 0 rather than NULL.

```sql
CREATE INDEX provider_calls_by_time      ON provider_calls (provider_id, called_at);
CREATE INDEX scene_items_by_scene        ON scene_items (scene_id, sort_order);
CREATE INDEX mission_attempts_by_mission ON mission_attempts (mission_id);
CREATE INDEX downloaded_areas_by_provider ON downloaded_areas (provider_id);
CREATE INDEX sessions_by_start           ON sessions (started_at);
```

Settings keys used so far (values are JSON): `ui.mainWindow.geometry` and
`ui.mainWindow.state` (base64 of `QMainWindow::saveGeometry()` / `saveState()`),
`display.units` (`"metric"` | `"imperial"`), `display.coordinates` (`"utm"` | `"utm+latlon"`),
`scene.terrain` (object: `preset`, `source` = `"procedural"`, `params`, optional `extent`
{`widthM`, `lengthM`}, `origin` {`lat`, `lon`, `alt`}) and `scene.environment` (object:
`time` ISO 8601 UTC, `timeZone` IANA name, `animate`, `timeRate`, `exposureMode`
`"auto"`|`"manual"`, `manualEv100`, `compensationEv`, `visibilityM`). The procedural `params`
object (also stored verbatim in `terrain_presets.params_json`) has `seed` (decimal string,
64 bit), `octaves`, `featureSizeM`, `lacunarity`, `persistence`, `amplitudeM`, `baseHeightM`,
`mountains`, `warp`, `erosion`, `edge` (`"cliff"`|`"fade"`|`"water"`), optional
`waterLevelM`, `snowLineM` and `flatZones` [{`x`, `z`, `radiusM`, `blendM`}]. User presets
have keys `user.<slug of the name>`; built-in presets (`builtin.*`) are defined in code.

### 4.11 GUI / Application Layer (`src/app`, Qt6)

**Main window layout:**

```
┌──────────────────────────────────────────────────────────────────────┐
│ Menu bar: File  Missions  Simulation  Vehicle  View  Help            │
├──────────────────────────────────────────────┬───────────────────────┤
│                                              │┌───┬───┬───┬───┬───┬─┐│
│                                              ││Mis│Sim│Veh│Ter│Itm│…││
│          3D Viewport (QOpenGLWidget)         │└───┴───┴───┴───┴───┴─┘│
│     Terrain + Sky + Items + Drone + HUD      │                       │
│                                              │   Active tab content  │
│                                              │                       │
├──────────────────────────────────────────────┴───────────────────────┤
│ Status: UTM pos | elev AMSL/AGL | state | mission | FPS | net/offline│
└──────────────────────────────────────────────────────────────────────┘
```

The viewport always shows the **position panel** (FR-GFX-14a), e.g.:

```
┌─────────────────────────────────────┐
│ UTM 30T  440 123.4 E  4 474 567.8 N │
│ Elev 712.4 m AMSL   AGL 35.2 m      │
└─────────────────────────────────────┘
```

and, at the bottom centre, the **compass** (FR-GFX-14b): true north at the top, the blue
magnetic-north marker turned by the declination, and the drone's heading arrow:

```
        MN
        ▼
     ·  N  ·        HDG 090°T  089°M   (top centre)
   W    ➤    E
     ·  S  ·
   DECL 1.2°E
```

The tab panel is a dockable `QDockWidget` containing a `QTabWidget`
(hidden for full-screen flying; `F11` toggles full screen, `F9` toggles the panel — `Tab` is not
used because it is needed for keyboard focus navigation). In full screen the panel is always
docked inside the window: a floating panel is docked when the window goes full screen and floats
again where it was afterwards (a separate window next to a full-screen one flickers, as the window
manager hands the focus back and forth).

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-UI-01 | Main window with the 3D viewport as central widget, requesting an OpenGL 4.5 Core context via `QSurfaceFormat`. | M |
| FR-UI-02 | Tab panel with the tabs below. | M |
| FR-UI-03 | Window geometry, dock state, last mission/scene restored on startup (from `settings`). | M |
| FR-UI-04 | Full-screen mode and panel hide/show; panel auto-hides when a mission starts. | M |
| FR-UI-05 | Mouse capture in the viewport for camera control, released with `Esc`. | M |
| FR-UI-06 | GUI changes are applied to Core through a thin controller/adapter layer — widgets never touch Core internals directly. | M |
| FR-UI-07 | Mission flow dialogs: **Briefing** (map, objectives, rules, weather, drone) → fly → **Debrief** (results, stars, retry / next / back). | M |
| FR-UI-08 | Long operations (area download, mission load) show progress and are cancellable; ongoing background downloads are shown in the status bar. | M |
| FR-UI-07a | Free mode: the user chooses the vehicle type and profile before starting (aerial or ground). | M |
| FR-UI-08a | Status bar shows drone UTM position and elevation (FR-GFX-14a), plus network state (online / offline / provider errors). | M |
| FR-UI-09 | Unsaved-changes prompt on scene switch and on exit. | S |
| FR-UI-10 | Light/dark theme following the system. | C |

**Tabs:**

| Tab | Contents |
|-----|----------|
| **Missions** | Campaign chapters and mission list with vehicle-type icon, lock state, stars, best time; mission details; *Start*; *Import mission*; *Editor* (S). |
| **Simulation** | Free flight/drive controls: vehicle type, Start / Pause / Step / Reset; time scale; flight or drive mode; arming state; camera mode; live telemetry (aerial: altitudes, speeds, attitude, motor RPM; ground: speed, heading, pitch/roll, wheel speeds/slip; both: battery V/A/%, GPS). |
| **Vehicle** | **Type: Aerial / Ground**, then profile select / create / duplicate / delete. *Aerial:* frame and mass, motors (KV, …), props (D, pitch, C_T/C_Q), battery, sensors noise, PID gains, rates/expo; *Apply*, *Reset to reference*. Derived stats: hover throttle, thrust-to-weight, estimated hover time, max wind resistance. *Ground:* chassis, wheels/tracks, suspension, tyres, drivetrain, battery, sensors, controller gains; derived stats per FR-GND-13. |
| **Terrain** | Preset selector; **Source: Procedural / Online / File (S)**; **Dimensions:** *Unlimited* or *Custom* width × length (disabled while a mission that sets its own extent is loaded). *Procedural:* seed + **Random**, noise and feature parameters, geo origin. *Online:* elevation provider, origin (lat/lon or UTM entry), max resolution, edge mode, exaggeration, **Download area** with size estimate; metadata panel (provider, CRS, coverage, resolution, cached size, min/max, attribution). *File (S):* open raster (COG, GeoTIFF, `.hgt`, `.asc`, `.img`) or LAS/LAZ files, band (rasters) or resolution/interpolation (LAS/LAZ, DTM), origin, sub-area; metadata (format, CRS, extent, resolution, point count and classes for lidar, NoData %, datum). *Orthophotos (S):* add orthophoto files over file or online elevation; coverage and resolution shown. *Regenerate*, 2D height preview. |
| **Items** | Item tree/list; add-item palette; property editor (name, transform or lat/lon/alt + AGL/AMSL, type props); snap-to-terrain; delete/duplicate; gate order editor. |
| **Environment** | **Date and time** (local time with time zone, or UTC), *Now* button, time-of-day slider; read-out of sun and moon (azimuth/elevation, moon phase and illumination), sunrise/sunset, moonrise/moonset and twilight times; light-pollution level (S); exposure (auto/manual). **Weather generator:** preset, seed + *Randomise*, temperature, humidity, QNH, **wind level slider 0–10** with name and resulting speed/gusts (e.g. "4 – Moderate breeze, 6.7 m/s, gusts 9.5 m/s") and direction, cloud layers, precipitation, visibility/fog, timeline on/off; *Advanced* custom wind values (S). |
| **Settings** | Graphics (resolution scale, MSAA, V-Sync, shadows, LOD bias, view distance, FOV, FPV uptilt); **Controls** (RC device, calibration wizard, channel map, switches, deadzones, rates); units (metric/imperial), coordinate display (UTM only / UTM + lat/lon); **Data providers** (list, add/edit URL template and protocol, API key, test connection, attribution/licence, usage stats); **Cache** (size limit, usage, pinned areas, clear, offline mode, proxy); data (open DB folder, export/import, clear history). |
| **History** | Flight sessions and mission attempts with stats; telemetry plots (S) drawn with `QPainter` or an LGPL/MIT plotting widget — **not Qt Charts** (GPL-only in open source, see §12). |

---

## 5. Cross-Cutting Concerns

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-X-01 | **Logging:** levelled logger (trace…error) to console and rotating file in `$HOME/.drone_simulation/logs/`. GDAL, libcurl and SDL log callbacks routed to it; API keys redacted. | M |
| FR-X-02 | **Error handling:** `std::expected` for recoverable errors in Core; exceptions only at module boundaries / startup; no exceptions across GL/Qt/GDAL/curl/SDL callback boundaries. | M |
| FR-X-03 | **Configuration:** command-line flags `--headless`, `--mission <key>`, `--scene <name>`, `--db <path>`, `--offline`, `--validate-mission <file>`, `--version`, `--log-level`, `--gl-debug`. | S |
| FR-X-04 | **Units & frames:** SI internally (metres, seconds, kg, radians, volts, amps); world frame right-handed **Y-up** (`x = East`, `y = Up`, `z = −North`); geo coordinates in `double`. | M |
| FR-X-05 | **Determinism:** with the same terrain source, mission, drone profile, terrain data hash, weather seed and settings, RNG seed (turbulence, sensor noise) and recorded input, a headless run produces an identical final state. Needed for fair scoring, replays and regression tests. | M |
| FR-X-06 | **Replay:** record input stream + seeds per session/attempt and replay it. | S |

---

## 6. Non-Functional Requirements

### 6.1 Realism / Fidelity
| ID | Requirement |
|----|-------------|
| NFR-REAL-01 | For each reference profile, simulated **hover throttle, max thrust-to-weight, hover endurance and max level speed** are within **±10 %** of values derived from the published component data (documented in `docs/FLIGHT_MODEL.md`). |
| NFR-REAL-02 | Free fall (motors off, no drag) matches `h = ½gt²` within 0.1 % over 5 s. |
| NFR-REAL-03 | Thrust at 3 000 m AMSL (ISA) is reduced versus sea level by the air-density ratio within 1 %. |
| NFR-REAL-04 | Step response of the default attitude controller: no sustained oscillation; settling time and overshoot documented per profile. |
| NFR-REAL-05 | Online terrain heights match the decoded provider data within the resampling error (≤ 0.5 × local height difference between adjacent samples) at sample points. |
| NFR-REAL-09 | Ground drone reference profiles: top speed, acceleration and maximum climbable slope within **±10 %** of values derived from the profile specs; static rollover angle within **±2°** of the analytical value (`atan(track / (2 · CoG height))`); braking distance on each material within ±10 % of `v² / (2 μ g)`. Documented in `docs/GROUND_MODEL.md`. |
| NFR-REAL-08 | In steady wind, the tilt angle and power needed to hold position match the analytical drag balance for the reference profiles within ±10 %; each profile's maximum wind resistance is documented in `docs/FLIGHT_MODEL.md`. |
| NFR-REAL-10 | **Astronomy accuracy** (years 1950–2100, any location): sun apparent position within **0.01°**; moon apparent (topocentric) position within **0.05°**; moon illuminated fraction within 1 %; bright-limb angle within 2°; sunrise/sunset within **1 min** and moonrise/moonset within **2 min** of reference data (JPL Horizons / USNO); star positions within 0.05°. |
| NFR-REAL-11 | Battery capacity and voltage sag vs. temperature follow the cell datasheet curves within ±10 %; fog/visibility: a dark object is just distinguishable at the configured visibility distance (Koschmieder relation) within ±10 %. |
| NFR-REAL-12 | Lidar DTM: for a synthetic point cloud sampled from a known surface, the rasterized heights match the surface within 5 cm RMS at 1 m resolution (TIN). |
| NFR-REAL-07 | Displayed UTM coordinates match reference conversions within 1 cm; displayed elevation AMSL matches terrain-source datum + drone height within 1 cm. |
| NFR-REAL-06 | Geo conversions round-trip (geodetic → ENU → geodetic) within 1 mm inside 50 km of the origin. |

### 6.2 Performance
| ID | Requirement |
|----|-------------|
| NFR-PERF-01 | ≥ **60 FPS** at 1920×1080 with default settings on a mid-range GPU (GTX 1060 / RX 580 class or newer), procedural or online terrain, **by day and by night** (stars, moon and vehicle lights on) and **in bad weather** (heavy rain, broken clouds); volumetric clouds have a quality setting. |
| NFR-PERF-02 | Simulation sustains **500 Hz** with ≤ 0.5 ms average tick time for one drone (aerial or ground), ≤ 500 items and an active mission. |
| NFR-PERF-03 | Terrain chunk (129×129, LOD 0) ≤ 10 ms on one worker (procedural, or decode + reproject of cached online tiles). |
| NFR-PERF-04 | Decoding + reprojecting one 256×256 (or 512×512) elevation tile ≤ 5 ms; opening a 1 GB COG to first rendered frame ≤ 5 s; **rasterizing 100 million LAZ points to a 1 m DTM ≤ 90 s** on an 8-core CPU (first load only) and reloading the cached result ≤ 3 s; with a warm cache, online-terrain mission start ≤ 3 s; network and disk latency never block the render or simulation thread; in-memory terrain cache bounded (default 512 MB). |
| NFR-PERF-04a | Orthophotos: 60 FPS maintained while flying low and fast over a 10 GB, 0.25 m orthophoto mosaic; GPU memory stays within the orthophoto budget (default 1 GB); sharp imagery appears within 1 s of arriving over a new area. |
| NFR-PERF-05 | No frame-time spikes > 33 ms caused by terrain streaming, network/cache I/O or DB writes. |
| NFR-PERF-06 | Cold start to interactive main window ≤ 3 s; mission start (procedural) ≤ 3 s. |
| NFR-PERF-07 | Memory ≤ 2 GB RSS at default view distance. |

### 6.3 Reliability
| ID | Requirement |
|----|-------------|
| NFR-REL-01 | The DB must never be left corrupted: all multi-statement writes use transactions. |
| NFR-REL-02 | NaN/Inf in physics state is detected, logged, and handled (drone reset in free mode, attempt marked failed in missions) — never a crash. |
| NFR-REL-03 | Malformed, truncated or unexpected API responses, malformed DEM files and malformed mission files never crash the app (fuzz-tested decoders/parsers). |
| NFR-REL-03a | Network loss, provider outages, rate limiting and expired/invalid API keys degrade gracefully (cached/coarser data, clear UI message), never crash or freeze. |
| NFR-REL-04 | RC device disconnect mid-flight triggers the configured failsafe, not a crash. |
| NFR-REL-05 | Zero leaks / UB reported by ASan + UBSan on the test suite. |

### 6.4 Maintainability & Quality
| ID | Requirement |
|----|-------------|
| NFR-MNT-01 | Core libraries have **no Qt dependency**, except `ds_storage` (Qt6::Core + Qt6::Sql only); no Qt, GDAL or SDL headers in any Core public header. Enforced by CMake target linkage. |
| NFR-MNT-02 | Unit-test line coverage of Core ≥ **80 %** (gcov/llvm-cov). |
| NFR-MNT-03 | `clang-format` and `clang-tidy` configs checked in; CI fails on violations. |
| NFR-MNT-04 | Compiler warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, treated as errors in CI. |
| NFR-MNT-05 | Every public class/function in Core has a Doxygen comment. |
| NFR-MNT-06 | Mission JSON schema and drone-profile schema are versioned; old versions are migrated or rejected with a clear message. |
| NFR-MNT-07 | Every source file starts with SPDX headers (licence identifier `MIT` and file copyright text, see §12.1); the repository follows the REUSE specification and CI checks it (`reuse lint`). |
| NFR-MNT-08 | New dependencies and assets must pass the license policy in §12 before being added. |

### 6.5 Usability
| ID | Requirement |
|----|-------------|
| NFR-USE-01 | First launch: reference drones, the built-in mission pack and a default procedural scene exist; the first tutorial mission starts within 2 clicks. |
| NFR-USE-02 | First-time RC setup guided by the calibration wizard in ≤ 2 minutes. |
| NFR-USE-03 | All numeric parameters show units and valid ranges; invalid input is rejected in the widget. |
| NFR-USE-04 | Keyboard shortcuts and RC switch functions listed in *Help → Controls*. |

### 6.6 Security & Privacy
| ID | Requirement |
|----|-------------|
| NFR-SEC-01 | `$HOME/.drone_simulation` created with permissions `0700`; DB file `0600`. |
| NFR-SEC-02 | Network access **only** to user-configured data providers, over HTTPS with certificate verification. GDAL's own network access is disabled. No telemetry, usage data or personal data leaves the machine. |
| NFR-SEC-02a | API keys stored in the OS keyring when available; never written to logs, crash reports, exported scenes/missions or the cache. |
| NFR-SEC-03 | Imported scenes, missions, **elevation and orthophoto files** and **all API responses** are untrusted: validated (schema, numeric ranges, content type, raster dimensions, decompressed-size limits) before use; file paths are confined to the mission folder or geodata library (FR-TER-F03). |

---

## 7. Build System

- **CMake ≥ 3.28**, generator **Ninja** (`-G Ninja`), with `CMakePresets.json`:
  `debug`, `release`, `relwithdebinfo`, `asan` (ASan + UBSan), `coverage`.
- Targets:

| Target | Type | Links |
|--------|------|-------|
| `ds_common` | static lib | glm, nlohmann_json |
| `ds_geo` | static lib | ds_common |
| `ds_astro` | static lib | ds_common, ds_geo |
| `ds_storage` | static lib | ds_common, Qt6::Core, Qt6::Sql |
| `ds_net` | static lib | ds_common, CURL::libcurl |
| `ds_terrain` | static lib | ds_common, ds_geo, ds_net, GDAL::GDAL, PDAL (pdalcpp) |
| `ds_items` | static lib | ds_common |
| `ds_input` | static lib | ds_common, SDL3::SDL3 |
| `ds_physics` | static lib | ds_common, ds_geo, ds_terrain, ds_items |
| `ds_missions` | static lib | ds_common (incl. its JSON Schema validator), ds_geo, ds_astro, ds_terrain, ds_items, ds_physics |
| `ds_graphics` | static lib | ds_common, ds_geo, ds_astro, glad, OpenGL::GL |
| `ds_sim` | static lib | all of the above (Qt only transitively via ds_storage) |
| `drone_simulator` | executable | ds_sim, Qt6::Widgets, Qt6::OpenGLWidgets |
| `unit_tests` | executable | Core libs, GTest |
| `integration_tests` | executable | Core libs, GTest (+ Qt6::Test for GUI smoke tests) |
| `docs` | custom | Doxygen |

- System dependencies (`find_package`): Qt6 (`Core`, `Sql`, `Gui`, `Widgets`,
  `OpenGLWidgets`, `Test`), OpenGL, **GDAL ≥ 3.6** (brings PROJ), **LASzip ≥ 3.4** (LAZ; PDAL is not packaged in Debian 13, see the M4 notes), **libcurl**, **SDL3**,
  **Assimp ≥ 5.2** (glTF 2.0 / OBJ model files, FR-GFX-12);
  QtKeychain (app layer, for the OS keyring).
  SQLite comes with Qt's `QSQLITE` driver plugin. glm, GoogleTest and nlohmann/json via
  `FetchContent` if not found; cpp-httplib (tests only, mock provider server); glad vendored in `third_party/`.
  Debian 13 packages: `qt6-base-dev libgdal-dev liblaszip-dev libcurl4-openssl-dev libsdl3-dev libassimp-dev qtkeychain-qt6-dev`.
- Shaders, models, reference vehicles, missions and provider presets are copied next to the
  binary at build time and installed under `<prefix>/share/` (see §7.1).
- `cmake --install` produces a relocatable layout: the app finds its data relative to its
  own executable (`<bin>/../share/…`), never through a hard-coded path, so it runs the same
  from the build tree and from `/opt/drone_simulator`.

Typical workflow:
```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --build --preset debug --target docs
cmake --build --preset release --target package   # → drone-simulator_<version>-1_amd64.deb
```

### 7.1 Binary distribution: `.deb` package

The first binary release format is a **Debian package** that installs into
**`/opt/drone_simulator`**.

| ID | Requirement | Pri |
|----|-------------|-----|
| FR-PKG-01 | Built with **CPack (DEB generator)** from the `release` preset: `cmake --build --preset release --target package`. | M |
| FR-PKG-02 | Package name **`drone-simulator`** (Debian package names cannot contain `_`), version `X.Y.Z-N` (SemVer + package revision), architecture `amd64`, section `games`, maintainer *Juan Maria Gomez Lopez*, homepage and description filled in. | M |
| FR-PKG-03 | **Install prefix `/opt/drone_simulator`** (`CPACK_PACKAGING_INSTALL_PREFIX`). | M |
| FR-PKG-04 | **Dependencies come from Debian** (target: **Debian 13 "trixie"**): computed with `dpkg-shlibdeps` (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`) plus explicit `Depends` for things it can't detect: the Qt SQLite plugin (`libqt6sql6-sqlite`), `ca-certificates` (HTTPS) and the OpenGL runtime. Qt, GDAL, PDAL, libcurl, SDL3 and QtKeychain are **not bundled**. | M |
| FR-PKG-05 | Desktop integration outside `/opt`: `/usr/share/applications/drone-simulator.desktop`, icons in `/usr/share/icons/hicolor/…`, a `/usr/bin/drone-simulator` symlink to `/opt/drone_simulator/bin/drone_simulator`, and `/usr/share/doc/drone-simulator/copyright` in Debian machine-readable format (DEP-5). | M |
| FR-PKG-06 | No maintainer scripts beyond what's strictly needed; the package **never touches users' `~/.drone_simulation`**. Removing or purging the package leaves user data in place (documented). | M |
| FR-PKG-07 | `lintian` runs in CI; the expected `/opt` warning is documented as an override. (Debian policy reserves `/opt` for add-on software, so this package is meant for our own distribution, not the official Debian archive.) | S |
| FR-PKG-08 | Releases publish the `.deb` with `SHA256SUMS`, signed with GPG (S); an APT repository (C); other distributions (Ubuntu 25.04+, which has SDL3) (S); `arm64` (C). | S / C |

**Installed layout:**
```
/opt/drone_simulator/
├── bin/drone_simulator
├── lib/                       # project-private libraries only (normally empty; RPATH $ORIGIN/../lib)
└── share/
    ├── assets/                # shaders, models, textures, sky, environment
    ├── vehicles/              # reference aerial and ground profiles
    ├── missions/              # built-in missions (files, or folders with dem/) + mission.schema.json
    ├── providers/             # data-provider presets (OpenTopography)
    └── doc/                   # LICENSE, THIRD_PARTY_NOTICES.md, user guide
/usr/bin/drone-simulator -> /opt/drone_simulator/bin/drone_simulator
/usr/share/applications/drone-simulator.desktop
/usr/share/icons/hicolor/*/apps/drone-simulator.{png,svg}
/usr/share/doc/drone-simulator/copyright
```

---

## 8. Testing Strategy

### 8.1 Unit tests (`tests/unit`, GoogleTest, no GPU, no Qt GUI)
| Area | Examples |
|------|----------|
| common | Math helpers, quaternion conversions, event bus dispatch, triple buffer, JSON helpers. |
| geo | Known geodetic ↔ ECEF ↔ ENU reference points; round-trip error < 1 mm; **UTM reference points** (both hemispheres, zone edges, Norway/Svalbard exceptions, band letters) within 1 mm; Web-Mercator tile math; distance/bearing. |
| astro | Fixture tables generated offline from JPL Horizons / USNO (no network in tests): sun and moon azimuth/elevation at many places and dates (both hemispheres, equator, polar day/night, near the horizon); moon phase vs. known full/new moon dates; bright-limb angle; rise/set/twilight times; DST changes in local time; Polaris altitude ≈ latitude; sidereal time; ΔT. |
| terrain — procedural | Same seed ⇒ identical heights; different seeds differ; chunk borders continuous; LOD stitching indices valid; parameter validation; bounded vs. unbounded extent (no chunks requested outside a set extent, streaming far from origin keeps precision). |
| terrain — rasters | Fixtures generated in-test with GDAL (`MEM` → COG, GeoTIFF, `.hgt`, `.asc`, `.img`): known values read back exactly at sample centres in every format; UTM vs. EPSG:4326 files give the same ENU heights; NoData filled; scale/offset and feet → metres; overviews used when present; memory limit respected; multi-tile mosaic (incl. mixed raster formats) is seamless; sub-area inside/outside; **path rules** (relative OK, `library:` OK, absolute, `..` and escaping symlinks rejected); `sha256` mismatch; missing/corrupt/unsupported files → correct `TerrainError`. |
| terrain — LAS/LAZ | Fixtures written in-test with PDAL (LAS 1.2 and 1.4, LAZ): points sampled from a known plane/hill give heights within tolerance (TIN, IDW); DTM ignores non-ground classes; `surface: "dsm"` rejected with a clear "not supported in v1.0" message; withheld and noise points dropped; holes filled up to the max size; missing CRS without `crs` rejected; unclassified cloud → clear error (or SMRF when `classifyGround`); multi-tile seams continuous; second load hits the cache (no re-rasterization); cancel leaves no partial cache entry. |
| terrain — orthophotos | Fixtures generated in-test with GDAL (RGB COG/GeoTIFF, RGBA, 4-band with `bands`, JP2 and PNG + world file): pixel colours land at the right ENU positions (≤ 1 px error); mosaic seams; NoData/alpha and out-of-coverage fall back to materials; cached COG created for files without overviews and reused; GPU budget respected by the streamer; procedural + orthophoto rejected; broken orthophoto disables imagery but the mission loads. |
| terrain — online | Terrarium / Terrain-RGB decoding exact for known pixels; GeoTIFF payload via `/vsimem/`; EPSG:3857 tiles and EPSG:4326/UTM coverages give the same ENU heights; URL-template expansion (placeholders, escaping, key injection); LOD → zoom selection; coarse-to-fine replacement; NoData/failed-tile fill; extent outside coverage rejected; malformed payloads → correct `TerrainError`. |
| weather | Preset expansion; same seed ⇒ same weather; timeline blending is smooth; plausibility checks; air density from temperature/pressure/humidity; battery capacity vs. temperature; grip changes for wet/snow/mud; visibility (Koschmieder). |
| net | Against an in-process mock server (cpp-httplib), including an **OpenTopography-like endpoint** (bbox → GeoTIFF, key parameter, quota-exceeded response): priority ordering, cancellation, timeouts, retries/backoff, `429` + `Retry-After`, circuit breaker, redirects/size limits, content-type rejection, ETag revalidation, cache hit/miss, LRU eviction, pinning, provider `allowCache=false`, offline mode, **API key never appears in logs**; block planning uses the minimum number of calls; rolling 24 h quota counting; no automatic retry after a quota error. |
| physics | Free fall; hover equilibrium; motor/prop thrust vs. hand-computed values; ISA density at altitude; battery sag vs. load; ground-effect gain; **wind levels:** level → parameter table, mean wind measured over 60 s at 10 m AGL within ±5 % of the table, level 0 gives exactly zero wind, logarithmic height profile, Dryden turbulence statistics (variance within tolerance), gusts deterministic for a given seed; payload changes mass/CoG; collision stops descent; crash threshold; NaN guard. |
| flight controller | PID step response; mixer saturation/airmode; mode transitions; pre-arm checks; failsafe. |
| ground drone | Suspension rest compression = `m·g / k`; constant torque on flat ground gives the expected acceleration minus rolling resistance; skid-steer turn rate and Ackermann turning radius match geometry; slip and climb limit per material (`tan θ ≈ μ` for traction-limited cases); braking distance; static rollover angle; stuck and water detection; speed-control and failsafe behaviour. |
| input | Calibration math, channel mapping, deadzone/expo curves (no device needed). |
| items | Add/remove/lookup; stale handle; spatial queries; gate direction detection; zone enter/exit; undo/redo. |
| missions | Schema validation (valid + many invalid fixtures, including objectives not allowed for the vehicle type, unknown properties, wrong `schemaVersion`); error messages contain the JSON path; schema migration of older versions; folder rescan detects added/changed/removed files; objective state machines for every type; dependency ordering; every failure condition; scoring and star thresholds; geo-coordinate item placement; rules enforcement (time scale locked, keyboard disallowed); **flight rules**: defaults applied when `rules` is omitted, 120 m AGL measured over sloping terrain, warning then failure after the grace time, VLOS distance limit reduced by fog, hidden-behind-terrain/building detection and `maxHiddenS`, lights required at night, overrides (`null` / `enabled: false`) and their briefing listing, ground missions without height limit, free mode warns only. |
| sim | Fixed-step accumulator; pause/step semantics; catch-up clamp. |
| storage | In-memory QSQLITE DB (`setDatabaseName(":memory:")`); test `main()` creates a `QCoreApplication` so driver plugins load: migrations empty → latest; CRUD round-trips; `CHECK` constraints on terrain presets; `mission_progress` view; built-in sync does not overwrite user entries; transaction rollback. |

### 8.2 Integration tests (`tests/integration`)
| Test | Description |
|------|-------------|
| First launch | Temporary `HOME`; verify `~/.drone_simulation/` and DB with correct permissions; reference drones and built-in missions synced. |
| Migration | v1 fixture DB migrated to latest; data preserved; backup created. |
| Scene round-trip | Scene with procedural preset + items → save → reload in a fresh process → identical state. |
| File terrain missions | Built-in mission folders with a COG, a non-COG raster and LAZ tiles, at least one with orthophoto files: load offline, physics `height()` matches the rendered mesh, items placed by lat/lon/UTM land at the expected positions, attempts record the combined data hash. |
| Online terrain scene | Mock provider server serving fixture tiles (elevation + orthophoto): stream chunks along a flight path; physics `height()` matches rendered mesh; items placed by lat/lon land at expected ENU positions; area download + restart in offline mode yields identical terrain. **CI never contacts real providers.** |
| Provider smoke (S, nightly, not gating) | Fetch one tile from each built-in preset to detect API changes or outages. |
| Headless flight | 30 s sim time with scripted input: arm, take off, position hold in wind, land; no crash; battery consumption within expected range. |
| Ground drive | Headless, each ground reference profile: drive a scripted route over procedural hills, climb a ramp, turn at speed; no rollover below the limit, rollover above it; odometry drift within tolerance; ground missions completed by their reference input. |
| Wind levels | For each aerial reference profile and levels 0–10: hover in Position hold for 60 s. Position error and power use grow with the level; the drone holds position (≤ 2 m error) up to its documented maximum wind resistance and drifts beyond it; Angle mode with centred sticks drifts downwind. |
| Headless missions | Every built-in mission has a recorded **reference input** that completes it in a headless run (guards against unwinnable missions); a deliberately bad input fails with the expected reason. |
| Determinism | Two headless runs of a mission with the same seeds + input produce identical final state hashes and scores. |
| Rendering (offscreen) | GL 4.5 context (EGL surfaceless / `QOffscreenSurface` under `xvfb-run`); render fixed procedural and mock-online scenes at **noon, sunset, full-moon night and moonless night**, and in **overcast, heavy rain and fog** (with the weather, M7); check **image properties** (sky colour and brightness, warmer horizon towards the setting sun, moon disc brightness and lit side, star count, exposure in range) rather than golden images, which differ between GPU drivers and Mesa versions. Also: every shader compiles, shader errors name file and line, frame time at 1920×1080 (asserted on hardware renderers only). Skipped if GL 4.5 unavailable. |
| Mission files | Every built-in `.mission.json` validates against `mission.schema.json` (also via `--validate-mission`), and the example files in `docs/MISSIONS.md` stay valid. |
| GUI smoke | `QTest`: launch `MainWindow`, switch every tab, open briefing/debrief, switch terrain source procedural ↔ online (mock server), check the UTM/elevation panel updates, close without errors. |
| Fuzzing (S) | libFuzzer targets for the mission JSON parser, the raster and LAS/LAZ loaders and tile/coverage decoders on malformed inputs. |

### 8.3 CI
- Build matrix: GCC 13/14 and Clang 17/18 on Linux; Debug + Release.
- Package job: build the `.deb` in a Debian 13 container, install it in a **clean** Debian 13
  container with `apt install ./drone-simulator_*.deb`, run `drone-simulator --version`,
  `--validate-mission` on every built-in mission and a headless smoke run, then remove it
  and check nothing is left in `/opt/drone_simulator`.
- Jobs: build, unit tests, integration tests (under `xvfb-run` with Mesa llvmpipe ≥ GL 4.5),
  ASan/UBSan, coverage report, clang-tidy, docs build (warnings as errors).

---

## 9. Documentation

| Artifact | Location | Content |
|----------|----------|---------|
| API reference | `build/docs/html/` (Doxygen) | All public Core APIs; module groups via `@defgroup` (Graphics, Items, Terrain, Geo, Physics, Input, Missions, Simulation, Storage). Graphviz graphs if installed. |
| `README.md` | repo root | What it is, screenshots, build instructions, dependencies, quick start, license. |
| `LICENSE` | repo root | MIT license text with the copyright line. |
| `THIRD_PARTY_NOTICES.md` | repo root | Every bundled library/asset with its license and attribution (installed with binaries). |
| `docs/SPECIFICATION.md` | this file | Requirements and design. |
| `docs/ARCHITECTURE.md` | docs/ | Module diagram, threading model, data flow. |
| `docs/FLIGHT_MODEL.md` | docs/ | Aerial drone: equations, parameters, reference profiles and their validation data. |
| `docs/GROUND_MODEL.md` | docs/ | Ground drone: suspension, tyre/track and drivetrain models, reference profiles and validation data. |
| `docs/TERRAIN.md` | docs/ | Procedural parameters; elevation files: rasters (formats, converting to COG with `gdal_translate -of COG`, `gdalwarp`) and LAS/LAZ (classification, DTM vs. DSM, resolution, interpolation, PDAL pipelines for ground classification); orthophoto files (formats, preparing COGs with overviews, colour adjustments); mission folder layout and path rules; online providers (protocols, URL-template placeholders, OpenTopography setup, quota); CRS and vertical datums; cache and offline use. |
| `docs/ASTRONOMY.md` | docs/ | Time scales, algorithms (sun, moon, stars), accuracy and the reference data used for validation. |
| `docs/MISSIONS.md` | docs/ | Mission file format: field reference generated from `mission.schema.json`, objective/failure types per vehicle, scoring, examples, authoring and validation guide. |
| `docs/BUILDING.md` | docs/ | Per-platform build, presets, building the `.deb`, troubleshooting GL/GDAL/SDL. |
| `docs/WEATHER.md` | docs/ | Weather generator: presets, parameters, timeline, effects on vehicles, JSON reference. |
| `docs/USER_GUIDE.md` | docs/ | Installing the `.deb`, getting an OpenTopography API key, RC setup, controls, flight modes, missions, tabs, where data is stored. |
| `docs/DATABASE.md` | docs/ | Schema, migrations policy. |
| `docs/adr/` | docs/ | Architecture Decision Records (Qt SQL, GDAL, SDL3, RK4, reversed-Z, …). |

Doxygen is configured with `USE_MDFILE_AS_MAINPAGE = README.md` and includes `docs/*.md`
so Markdown pages appear in the generated site.

---

## 10. Milestones

| # | Milestone | Deliverables |
|---|-----------|--------------|
| M0 | **Skeleton** | CMake/Ninja presets, empty Core libs, Qt main window with GL 4.5 viewport, GoogleTest + CTest, Doxygen target, **CPack `.deb` skeleton installing to `/opt/drone_simulator`**, CI incl. install test. |
| M1 | **Storage + Geo + Astro** | `$HOME/.drone_simulation`, Qt SQL wrappers, migrations, settings persistence; `core/geo` conversions (ENU, UTM, Web Mercator); `core/astro` time scales, sun, moon, phase, rise/set, stars, validated against reference tables. |
| M2 | **Procedural terrain + day/night sky** | Height-source abstraction, procedural source, chunking/LOD, async generation, atmospheric sky lit by sun and moon, moon phases, stars, HDR + exposure, fog, reversed-Z + camera-relative rendering, free-fly camera, Terrain tab (procedural). |
| M3 | **Realistic flight** | `Vehicle` interface, RK4 rigid body, motor/prop/battery/atmosphere models, wind levels 0–10, sensors + estimator, flight controller (Acro/Angle/Alt hold), RC input via SDL3 + calibration, FPV/LOS/chase cameras, HUD with UTM/elevation position panel and compass (true and magnetic north, WMM2025), Drone + Simulation tabs, reference profiles. |
| M4 | **Real terrain (raster/LAS/LAZ files, orthophoto files + OpenTopography)** | Orthophoto files (mosaic, reprojection, LOD streaming, cache); raster source (COG, GeoTIFF, `.hgt`, `.asc`, `.img`; partial reads, overviews, mosaics), LAS/LAZ → DTM rasterization with PDAL and COG cache, path rules, validation, sha256, mission folders; `core/net` (client, cache, offline, credentials), OpenTopography preset (Global DEM + USGS APIs), block fetching + daily quota accounting, area download, attribution, Terrain tab (online), Settings → Data providers, mock-server tests. |
| M5 | **Items + Missions** | Items Manager with triggers, mission loader/validator, objectives, failures, scoring, briefing/debrief, campaign progress, built-in mission pack (≥ 10), Missions tab. |
| M6 | **Ground drone** | `Vehicle` interface shared by both types, ground physics (suspension, tyres, drivetrain, rollover, stuck, water), drive modes, ground input mapping, ground HUD/visuals, reference ground profiles, ground missions (≥ 4), parking zone item. |
| M7 | **Weather & fidelity** | Weather generator (presets, clouds, precipitation, fog, timeline) and its effects on vehicles, battery temperature, cloud/rain/fog rendering; Position hold / RTH, failure injection, validation against NFR-REAL, History tab, replays. *(The online orthophoto provider and further protocols (S) moved after M4.)* |
| M8 | **v1.0 release** | Performance targets, coverage ≥ 80 %, user/mission/terrain/weather docs, signed `.deb` release for Debian 13. |

**M2 implementation notes** (deliberate scope decisions, revisited in later milestones):

- **Shadows (FR-GFX-10)** and moon shadows (FR-GFX-08c) are not in M2: terrain is lit by the
  Sun, the Moon and the sky with N·L and without cast shadows. Cascaded shadow maps arrive
  with the vehicles in M3, where the drone's shadow matters.
- **Earth curvature:** terrain and water are drawn on the flat local tangent plane; the
  atmosphere (sky, aerial perspective, sun/moon transmittance) uses a spherical planet. At
  40 km the curvature drop (≈ 125 m) is hidden by haze; real-terrain scenes (M4) revisit it.
- **Sky model:** Bruneton transmittance table, Hillaire multiple-scattering and sky-view
  tables (azimuth from north, so the Sun and the Moon share one table), recomputed every
  frame for the camera altitude; aerial perspective uses the sky-view luminance scaled by the
  extinction in front of the surface (no froxel volume). Visibility sets the aerosol density
  at the ground with the Koschmieder relation (3.912 / V).
- **Night:** airglow and unresolved starlight give a background of 2.5·10⁻⁴ cd/m² (≈ 0.001 lux
  on the ground); stars are point sprites whose brightness is normalised to the pixel size
  of a 1080-line, 60° view, so the visible stars do not depend on the window size. The
  automatic exposure adds a night bias (up to +6 EV) so night looks dark, and the tone
  mapper blends towards a desaturated, slightly blue (scotopic) image below ~3 cd/m².
- **Moon disc:** Lommel–Seeliger reflectance lit along the Moon→Sun vector (exact phase and
  bright-limb orientation), NASA SVS/LROC albedo map oriented by the ecliptic pole (lunar
  axis within 1.5°; libration ignored), earthshine on the dark part.
- **Free mode camera:** until a drone exists (M3) the position read-out in the status bar
  follows the free-fly camera.

**M3 implementation notes** (deliberate scope decisions, revisited in later milestones):

- **Flight model:** see `docs/FLIGHT_MODEL.md` (frames, equations, reference profiles,
  validation figures, step responses and wind limits).
- **Vehicle visuals:** the drone is a procedural model built from the profile (frame, motors,
  propellers, FPV camera, landing feet, navigation lights); propellers become translucent
  blur discs above 15 % of maximum speed. Model files (glTF/OBJ) came in M6 (step 7). The navigation lights are emissive but do not light the scene (deferred to M7).
- **Shadows (FR-GFX-10):** three cascades (25 / 120 / 600 m, 2048² by default, texel-snapped)
  from the Sun, or from the Moon when the Sun is below the horizon; terrain and vehicles cast
  and receive. Translucent blur discs do not cast.
- **Cameras:** FPV (profile uptilt and field of view, rolls with the body), chase (behind the
  direction of travel, distance 14 × arm length clamped to 1.5–6 m, height 0.35 × distance,
  smoothed with τ = 0.25 s), line of sight (pilot eye 1.7 m above the ground, 4 m behind the
  take-off point) and the M2 free camera. `C` cycles; the choice is saved.
  The chase camera adds two insets in the bottom corners (22 % of the view width, 16:9, at most
  28 % of its height; not below 320 × 240): a **top view** straight down from 2 × the chase
  distance above the vehicle, north up, with a heading chevron marking the vehicle, and a
  **behind view** rigidly behind the nose (heading only, level horizon, unsmoothed) at
  0.4 × the chase distance with a 45° lens. The insets are rendered with the main view's
  terrain chunks and exposure (`Renderer::renderInset`); the HUD's bottom columns move above
  them.
- **Keyboard flying** works only in the vehicle cameras (in the free camera W/S/A/D move the
  camera). Menu shortcuts are function keys (F5 start, F6 pause, F7 step, F8 reset) because
  letters are flight keys in the 3D view.
- **Clock:** before the first Start the scene's clock (Environment tab) drives the sky and the
  vehicle's UTC; from then on the simulation clock drives the sky (time scale included) and a
  time edited in the Environment tab moves the simulation clock.
- **Position hold and return to home** (flight mode 4 and the RTH switch) arrived in M7 step 5
  (see the M7 notes). Each flight goes to the `sessions` table (flights shorter than 1 s
  armed are not recorded); since M7 step 7 its 20 Hz telemetry and its replay go with it.
- **Estimator without GPS:** the attitude estimate uses the accelerometer only within ±5 % of
  1 g (plus GPS-derived acceleration once there is a fix), so long accelerating manoeuvres
  without a fix drift slowly, as on a real flight controller.
- **Status bar:** shows the drone's position in the vehicle cameras, the camera's in the free
  camera.
- **Compass (FR-GFX-14b, FR-GEO-05):** the full WMM2025 model (degree 12) in `core/geo`,
  coefficients generated from NOAA's `WMM.COF` by `scripts/data/gen_wmm.py`, checked against
  all 100 official test values. The HUD heading is the true heading from the ground-truth
  attitude; the magnetic heading is derived from it with the declination at the drone (the
  simulated magnetometer still measures the true heading with noise — magnetic deviation and
  the magnetometer field model are M7 topics). Simulation dates outside 2025–2030 use the
  field at the nearest end of the period, flagged in amber.

**M4 implementation notes** (in progress; deliberate scope decisions):

- **Step 1 — network foundation and file plumbing (done):** `core/net` with the libcurl
  client (network thread, priorities, cancellation, per-provider concurrency and rate limit,
  HTTPS-only unless a provider is marked insecure, 10 s / 30 s timeouts, 5 redirects, response
  size and content-type limits, retries with equal-jitter backoff, `Retry-After`, circuit
  breaker, manual offline mode and unreachable-network detection); provider configuration
  (`assets/providers/*.json`, user providers in `data_providers`); URL templates with key
  injection and redaction; block planning (fewest requests within the area and response-size
  limits; fixed 20 km streaming grid); rolling 24 h quota over `provider_calls`; the disk cache
  (`cache/<provider>/…`, 0700/0600, SHA-256 checked on every read, LRU eviction, pinning, ETag /
  Last-Modified revalidation) indexed in the separate `cache.db`; area download with estimate,
  progress, cancel and pinning. `core/terrain`: the mission `terrain` object for
  `"source": "file"`, path rules (mission folder or `library:`, symlink escapes rejected),
  size limits, `sha256` checks and `terrain_data_hash`.
- **Quota counting:** every attempt that reaches the provider counts as a call, retries
  included; quota-limited providers therefore retry at most once. Before each request the
  remaining budget is checked; with none left the provider is served from the cache only.
- **OpenTopography preset:** the Global DEM request (`/API/globaldem?demtype=…&south=…&north=…
  &west=…&east=…&outputFormat=GTiff&API_Key=…`), the dataset area limits and the 50 calls / 24 h
  limit are verified against OpenTopography's published documentation. The USGS request
  (`/API/usgsdem?datasetName=…`), the response content types and the body of a quota-exceeded
  response are **not verified yet** (the API reference page is JavaScript-rendered); those
  datasets are marked `"verified": false` and must be checked before release.
- **Tests:** the network tests use a small in-process HTTP/1.1 server written for the test suite
  (`tests/unit/mock_http_server.hpp`) instead of cpp-httplib, so no extra test dependency is
  needed. CI never contacts real providers.
- **Plan for the remaining work** (dependencies: `libgdal-dev`, `liblaszip-dev`,
  `qtkeychain-qt6-dev`):
  1. network foundation (done);
  2. GDAL raster source;
  3. file terrain in the application: missions, Terrain tab, edge handling, attribution;
  4. online DEM source (OpenTopography) with block streaming and area download;
  5. Settings → Data providers with the OS keyring;
  6. LAS/LAZ → DTM with LASzip and the COG cache;
  7. orthophoto files with GPU streaming (done);
  8. the online orthophoto provider (moved here from M7 step 9: WMTS and XYZ/TMS (S), WMS
     GetMap (C), using the Web-Mercator tile math of FR-GEO-02b) (done);
  9. built-in file-terrain missions, integration tests and `docs/TERRAIN.md` (done).
- **LAS/LAZ without PDAL:** PDAL is not packaged in Debian 13. LAS and LAZ are read through
  LASzip (`liblaszip-dev`), and the DTM rasterization (TIN, IDW, hole filling) is project
  code. Automatic ground classification (`classifyGround`, SMRF, S) waits for a
  ground filter of its own; until then a cloud without ground points fails with a clear
  message (FR-TER-L04, M part).
- **Step 2 — GDAL raster source (done):** `terrain::RasterSource` opens COG / GeoTIFF,
  `.hgt`, `.asc` and `.img` files, alone or as a mosaic (an in-memory GDAL VRT).
  - *GDAL set-up:* every driver except the elevation and orthophoto ones and GDAL's own
    VRT / MEM / COG is deregistered, so GDAL has no network drivers. Network paths are refused,
    and no `.aux.xml` files are written into mission folders. The block cache is 256 MB.
  - *Validation:* data type (Int16, UInt16, Int32, Float32, Float64), band, size
    (40 000 px a side), georeferencing, CRS (or the mission's `crs`), one CRS for all files,
    unit (metres, feet, US survey feet), and the origin and extent inside the data. Each
    problem is a TerrainError with its own code (new: `MixedCrs`, `CorruptData`,
    `NotCovered`).
  - *Reprojection:* each world point goes to WGS84 through the scene's `LocalFrame`, at the
    origin altitude like the app's frame, then into the raster CRS with PROJ. Heights are
    bilinear between pixel centres. A plane in UTM is reproduced within 1 mm against the
    project's own UTM code.
  - *Levels of detail:* grids up to the level-0 spacing (2 m) read full-resolution pixels, so
    meshes and height queries agree bit for bit. Coarser grids read GDAL's average over
    aligned power-of-two blocks, from overviews when present. These agree between windows
    only to float rounding (≈ 1e-5 m), which is enough for render levels.
  - *NoData (FR-TER-C01):* a void pixel takes the inverse-distance mean of the valid pixels
    within 8 pixels, otherwise the lowest elevation of the data. Read errors in a damaged file
    count as NoData and are counted for the UI.
  - *Placement:* the default origin is the centre of the data, and an AGL origin takes the
    ground height there. Without a mission extent the terrain is the largest origin-centred
    rectangle the data covers, because `Extent` is always centred on the origin.
  - *Speed (release build):* 6 ms for a 131 × 131 chunk grid, 0.5 µs for a point query.
    Each thread reads through its own GDAL handle and PROJ transformation.
  - *Not done here:* the cached COG copy for large rasters without overviews (C02; it comes
    with the COG writer of step 6). Until then the metadata suggests converting such files
    with `gdal_translate -of COG`.
- **Step 3 — file terrain in the application (done):**
  - *Scene configuration:* `TerrainConfig` has a source kind (procedural or file) and a
    `FileTerrainConfig`: files, band, CRS, datum, attribution, optional origin and AMSL
    origin altitude, and edge. These are saved in `scene.terrain` and in terrain presets
    (source type `file`).
    - `buildTerrain()` builds either source. `SceneController` builds before it switches, so
      a terrain that fails to open changes nothing. A saved file terrain whose files have gone
      starts on the default preset and keeps the saved choice.
    - The origin of file terrain is filled in from the data. The renderer and the HUD take the
      extent from the source, not from the configuration.
  - *Missions:* the mission loader stores the canonical paths of the elevation files it
    found (`resolvedElevationFiles`), so the briefing map, the daylight text and the scene
    open them without the folders.
    - Raster missions are playable. LAS/LAZ ones are unavailable until step 6, online ones
      until step 4.
    - At launch the files are checked against their `sha256` and the terrain data hash is
      computed, on a worker thread behind a progress dialog with *Cancel*. Hashes are cached
      by path, size and modification time for the session.
    - The hash goes into `mission_attempts.terrain_data_hash`, and the debrief's personal
      best only compares attempts on the same data (FR-TER-O14, F09).
  - *Origin (FR-TER-09):* an AGL origin, or none, puts the ground at the origin at height 0.
    An AMSL origin altitude is kept.
  - *Edges (FR-TER-O08):* *clamp edge* (the mesh is closed by its wall) and *fade to the
    lowest height* within 500 m of the border (at most a tenth of the shorter side). The HUD
    shows a caution 300 m from the terrain edge and a warning 100 m from it; this soft
    geofence works on procedural terrain too. *Procedural fill* beyond the data is not done
    (S).
  - *Terrain tab (FR-TER-F11):* the *Elevation file* source has:
    - a file list with *Add…* / *Remove*, band, CRS, vertical datum, attribution and edge;
    - "centre of the data, at ground level" as the default origin;
    - a metadata view: format, CRS, size, resolution, height range, datum, extent, NoData
      and advice (FR-TER-O12).
  - *Attribution (FR-TER-O13, F10):* the terrain's attribution is drawn at the bottom right
    of the viewport. Debrief screenshots don't exist yet.
  - *Not done:* the DEM-file mission of the built-in pack and its licensed data (step 9);
    opening LAS/LAZ files from the Terrain tab (step 6).
- **Step 4 — online elevation terrain (done, FR-TER-O01, O09, O12–O18, FR-NET-07):**
  - *Bounded terrain (with an extent):* the area needed is the extent plus a 300 m margin.
    It is fetched in as few requests as the provider's limits allow (FR-TER-O16), and the
    pieces are opened from the cache as one surface in GDAL memory files.
    - Opening never uses the network. `ensureOnlineData()` first asks, with the number of
      calls and *"uses N of your M remaining calls today"* (FR-TER-O17). It then downloads
      behind a progress dialog with *Cancel*; the pieces are pinned in the cache.
    - The terrain data hash is the SHA-256 of the provider, dataset and piece hashes. A
      restart in offline mode opens the same data and gives identical heights.
  - *Streaming (no extent):* the world is cut into the provider's fixed blocks
    (`blockSizeKm`). The block under the origin must be downloaded first; the blocks within
    one block size of the camera follow in the background, at most two at a time.
    - Physics and rendering never wait. Where no block has arrived yet, the height is the
      lowest height of the origin block.
    - Each arrival raises the source's *revision* with the changed area. The streamer keeps
      drawing the old meshes there and rebuilds them (`StreamerStats::stale`).
    - Failed blocks are retried after 2 min, never when the quota is used up. The reason is
      shown in the metadata view.
  - *Missions:* online missions are playable when the provider and dataset are known and cover
    the origin, and when an API key is set or the area is already cached (FR-MIS-02).
    - Missions are always bounded: the mission's extent, or 10 km × 10 km. The area is
      downloaded at launch, before PreFlight.
    - The attempt records the terrain data hash (FR-TER-O14). The mission loader also knows
      the providers of the user database.
  - *API keys (FR-NET-07, interim):* `DS_API_KEY_<PROVIDER>`, e.g.
    `DS_API_KEY_OPENTOPOGRAPHY`, else the copy in the user database. Entering keys in the
    UI (keyring) is step 5.
  - *Terrain tab:* the *Online elevation* source has:
    - provider, dataset and edge;
    - *Download Area…* (also offline later);
    - a line with the calls left today, the key state and offline mode;
    - a privacy note.

    The metadata view shows the provider, protocol, dataset, resolution, datum, CRS,
    coverage, cached area or streaming blocks, height range and attribution (FR-TER-O12).
  - *Data services:* the tile cache index is `cache.db` next to the user database, in memory
    if the Qt SQLite driver is missing. The cache is at `~/.drone_simulation/cache` (5 GB).
  - *Not done:* Settings › Data providers and the keyring (step 5); procedural fill beyond
    the data (S).
- **Step 5 — Settings › Data providers and Cache (done, FR-NET-05 … 09, 11, FR-TER-O02):**
  - *API keys (FR-NET-07, NFR-SEC-02a):* keys are read from `DS_API_KEY_<PROVIDER>` first.
    That variable wins over a stored key, and its key cannot be changed in the UI.
    - Otherwise keys come from the OS keyring through QtKeychain, service `DroneSimulator`,
      entry `provider/<key>`. QtKeychain's plain-text fallback is off.
    - If the keyring fails, keys come from the user database, which is set to file mode 0600.
      The panel shows a warning with the keyring's error.
    - A key is never in both places: saving to the keyring removes the database copy.
    - Keys are trimmed. The stored key is never shown again.
    - Each keyring call runs a local event loop with a 20 s timeout. After one failure the
      keyring is not asked again in that session.
    - Tests use an in-memory keyring and never touch the real one.
  - *Providers (FR-TER-O02):* the list shows each provider's kind, key state and calls left
    today, with details: URL template, datasets, limits, attribution, licence and terms link.
    - User providers can be added, duplicated from a built-in one, edited and removed. The
      editor covers key, name, protocol, URL template, format, datasets, key placement, daily
      limit, concurrency, block size, caching, plain HTTP, attribution, licence and terms. It
      validates like a provider file and keeps the fields it does not show.
    - Built-in providers are read-only. Only `rest_bbox` and EPSG:4326 are offered for
      elevation; the other protocols are listed as not supported yet. Imagery providers
      (xyz, tms, wmts, wms in EPSG:3857) came with step 8.
    - Removing a provider also deletes its key and its cached data.
    - The mission loader, the Terrain tab and mission playability follow changes at once.
  - *Usage (FR-NET-09):* calls, failures and bytes over the last 24 h, the remaining budget,
    the session's cache hits versus requests (`DataClient::stats`), the cached and pinned size,
    and an open circuit breaker.
  - *Test connection (FR-NET-11):* one uncached request (`BBoxRequest::noCache`) for a 0.01°
    square at the centre of the dataset's coverage, else at 46.5° N, 8° E. The answer is
    decoded as an elevation raster.
    - The result shows the latency, size, raster size and height range, or the error.
    - Quota-limited providers ask first: *"uses 1 of your N remaining calls"*.
  - *Cache (FR-NET-05, 06, 08):* the panel shows usage and the size limit, and has *Release
    Downloaded Areas* (unpin, then the limit applies), *Clear Cache…* (keeps pinned areas),
    *Open Folder*, *Work offline* and the network state (unreachable ⇒ cache only).
    - The proxy is used from the next start.
    - Offline mode, the limit and the proxy are saved as `net.offline`, `net.cacheLimitMb` and
      `net.proxy`.
  - *Not done:* imagery providers (steps 7–8); proxy settings without a restart (S).
- **Step 6 — LAS / LAZ point clouds to a DTM (done, FR-TER-L01 … L06):**
  - *Reading (FR-TER-L01):* `ds/terrain/lidar.hpp` reads LAS 1.0–1.4 and LAZ with **LASzip**
    rather than PDAL (see above). LASzip is private to `ds_terrain`.
    - Points are streamed one at a time; only the grid is kept in memory.
    - Formats 6 and up use the extended classification and the withheld flag.
    - The CRS comes from the WKT record (2112) or the GeoTIFF keys (34735, EPSG codes only).
      Otherwise `crs` is needed. A geographic CRS is refused.
    - `VerticalUnitsGeoKey` feet and US survey feet are converted to metres.
    - COPC files read as plain LAZ (not tested).
  - *Rasterization (FR-TER-L02, L03):*
    - Ground (2) and water (9) points are averaged per cell.
    - The automatic cell size is about 2 × sqrt(area / points), rounded to 0.25 m, within
      0.5 … 20 m. It becomes coarser while the grid exceeds 32 M cells. An explicit
      `resolutionM` that is too fine gives TooLarge, with the cell size that would fit.
    - Gaps are found by flood fill. Gaps that touch the border, or are larger than 50 m on a
      side, stay NoData (−9999) and are filled by the raster reader.
    - The other gaps are filled from their rim cells. TIN uses a Bowyer–Watson triangulation
      with barycentric heights, and IDW (power 2) covers any cell no triangle reaches. Rims
      over 3000 cells are thinned evenly.
  - *Unclassified clouds (FR-TER-L04, M part):* no ground points gives NotSupported, with a
    message pointing to SMRF or `lasground`. `classifyGround` is accepted and reported, but
    there is no SMRF yet (S).
  - *Caching (FR-TER-L05):* the DTM is written as a COG (DEFLATE, predictor, 256-pixel blocks,
    AVERAGE overviews) to `cache/dem/<key>.tif`. It is written beside the target and renamed
    when complete, so a cancelled or failed run leaves no file.
    - Deviation from the requirement: the key is a SHA-256 of each file's canonical path, size
      and modification time, plus the settings (`lidarDemKey`), not of the file contents.
      Finding a cached DTM therefore never re-reads gigabytes. The attempt's
      `terrain_data_hash` still uses the content hashes (FR-TER-F09).
    - The summary (points, ground, dropped, filled and NoData cells, CRS, interpolation) is
      stored in the COG's metadata (`readLidarSummary`).
    - DTMs appear in the cache usage as `lidar-dtm` and are removed by *Clear Cache*. The size
      limit does not evict them, because rebuilding one can take minutes.
  - *Progress (FR-TER-L06):* `ensureLidarDem()` runs the rasterization on a worker thread
    behind a progress dialog with cancel.
    - It runs at mission launch, after the file hashes are checked, and when the Terrain tab
      applies a point-cloud source.
    - `buildTerrain()` only opens the cached DTM, so point-cloud terrain needs DataServices.
      Without them the mission is unavailable, with the reason "needs the data cache".
  - *Terrain tab:* *Add…* accepts `.las` / `.laz` and refuses to mix rasters and point
    clouds. A point cloud shows *DTM cell size* (automatic or a value) and *Gap filling* (TIN
    or IDW) instead of *Band*.
    - The metadata says "LAS/LAZ point cloud → DTM" and adds a line with the point counts
      and the filled and NoData cells.
    - The settings are saved in the scene and in presets: `resolutionM`, `interpolation` and
      `classifyGround`.
  - *Tests:* the fixtures are written in-test with LASzip from a known surface. They cover:
    - LAS 1.2, LAS 1.4 and LAZ, and a DTM within 0.25 m of the surface;
    - noise and withheld spikes ignored;
    - gaps filled by TIN and IDW, large gaps left as NoData;
    - seamless tiles;
    - the automatic cell size, coarsening and feet;
    - missing and mixed CRS, geographic CRS, unclassified clouds, a grid that is too large;
    - cancel leaving no file, and the cache key.
    - Integration tests cover the Terrain tab (made once, reused, a new DTM per setting,
      metadata, cache usage and clearing) and a LAZ mission flown and recorded with its hash.
      They also cover an unclassified mission refused with its message, and unavailability
      without the data cache.
- **Step 7 — Orthophoto files with GPU streaming (done, FR-TER-I01 … I10, FR-GFX-07):**
  - *Reading (FR-TER-I01, I04):* `ds/terrain/orthophoto.hpp` opens COG / GeoTIFF, JPEG 2000,
    and PNG / JPEG / WEBP with a world file, 8-bit only.
    - Bands: gray, gray + alpha, RGB or RGBA, or `orthophotoBands` (3 or 4 band numbers).
      A NoData value makes pixels with that value in every band transparent.
    - Several files become one in-memory VRT mosaic (`GDALBuildVRT`, `-addalpha`), so
      seams are continuous and gaps are transparent.
    - `orthophotoCrs` overrides the CRS of the files (`-a_srs`).
  - *Tiles and reprojection (FR-TER-I05):* the imagery is cut into tiles on the chunk
    lattice: 256 px for 64 m at level 0 (0.25 m texels), doubling per level, 4 × 4 tiles per
    chunk.
    - Each tile is reprojected on the fly: world → geodetic (the terrain's local frame) → the
      file's CRS, exact on a 17 × 17 lattice and bilinear between. The read uses the
      overview that matches the tile (`GRIORA_Average`).
    - Tests place pixels within 1 px of their world positions.
    - Alpha is feathered 2 texels inward, so the edge blends softly into the materials.
    - The tile is sRGB with premultiplied alpha and a CPU-built mip chain. A tile without
      imagery is not stored.
  - *Streaming (FR-TER-I06, NFR-PERF-04a):* `OrthoStreamer` builds the tiles of the drawn
    chunks on the shared worker pool.
    - It also requests their ancestors up to 3 levels, so coarser imagery stands in at once.
    - Nearer tiles (relative to their size) and coarser tiles are built first.
    - An LRU CPU cache of 384 MB keeps the built tiles.
    - On the GPU, `OrthoTextures` keeps tiles in one `GL_SRGB8_ALPHA8` texture array. The
      array grows by doubling up to the budget (`RenderSettings::orthoBudgetBytes`, 1 GB)
      and the driver's layer limit.
    - At most 16 tiles are uploaded per frame. Least recently used layers not needed this
      frame are reused.
    - When the visible tiles do not fit in ¾ of the layers, every chunk uses coarser tiles
      (a level bias), so the imagery stays complete.
    - The shader reads the tile with continuous gradients (`textureGrad`), so there are no
      seams between tiles, and with 8× anisotropy when available.
    - Release build, 0.25 m JPEG COG: about 4 ms per tile.
  - *Cache (FR-TER-I06):* files over 4096 px without overviews (and PNG / JPEG / WEBP over
    that size) are converted once to a COG with overviews (DEFLATE). The COG is written to
    `cache/ortho/<sha256 of path, size, mtime>.tif`, through a partial file and a rename.
    - `ensureOrthoCache()` shows progress with cancel, at mission launch and when the
      Terrain tab applies imagery.
    - The COGs are listed in the cache usage as `ortho-cog` and removed by *Clear Cache*.
  - *Lighting (FR-TER-I07):* the image replaces the material albedo
    (`albedo · (1 − a) + rgb`) and is lit like the rest of the terrain.
  - *Missions (FR-TER-I02, I08, I09):*
    - The loader resolves `orthophotoFiles` with the elevation path rules. A missing image
      does not make the mission unavailable.
    - `orthophotoColor` (`brightness`, `contrast`, `gamma`) is new in the schema.
    - At launch the images are hashed. An image whose `sha256` differs, or that cannot be
      opened, only disables the imagery, and the status bar says why.
    - The attribution is shown as "elevation · imagery". Orthophotos are not part of
      `terrain_data_hash`.
  - *Terrain tab (FR-TER-I10):* an *Orthophotos* group for file and online elevation:
    - a file list with *Add…* and *Remove*;
    - CRS, bands, attribution, and brightness / contrast / gamma;
    - a summary of format, size, resolution, bands, CRS and coverage in kilometres from the
      origin.
    - The imagery is saved with the scene and presets (key `ortho`), and ignored for
      procedural terrain.
    - Imagery that does not reach the terrain is kept, with the caution "the orthophotos do
      not cover the terrain".
  - *Deviations:*
    - Reprojection is done per tile at run time. A different CRS therefore never needs a
      converted copy; only size without overviews does.
    - 16-bit imagery (S) is refused as UnsupportedFormat. The scaling min / max is not
      implemented.
    - The per-mission 50 GB limit is not checked; the 200 000 px limit per side is.
    - Online-elevation *missions* cannot declare orthophoto files. The schema has them only in
      `terrainFile`; online missions take an imagery provider instead (step 8). The Terrain
      tab can drape files over online elevation.
  - *Tests:*
    - Unit: pixel placement, coverage and NoData fallback, seamless mosaics, band layouts,
      clear errors, PNG + world file, JP2, colour adjustments, the cached COG, and streamer
      priorities and eviction.
    - Configuration: JSON / preset round trip, a broken image disabling only the imagery,
      and `orthophotoColor` parsing.
    - GL: imagery draped within a GPU budget of 6 tiles.
    - Application: a mission with imagery and a mission with a damaged image, and the
      Terrain tab.

- **Step 8 — Online imagery providers (done, FR-TER-O10, FR-GEO-02b, FR-NET-11):**
  - *Providers:* a provider of `"kind": "imagery"` uses the protocol `xyz`, `tms`, `wmts`
    or `wms`, and its `crs` must be EPSG:3857.
    - Templates of the tile protocols need `{z}`, `{x}` and `{y}`. `tms` counts rows from
      the south.
    - WMS templates need `{bbox}`, the tile's EPSG:3857 bounds; `{width}` and `{height}` are
      `tileSizePx`, a power of two in 64 … 1024 (default 256).
    - Each layer (dataset) needs `maxZoom`, and may give `minZoom`. Its `resolutionM`
      defaults to the pixel size at `maxZoom`.
    - Elevation providers keep `rest_bbox` and refuse the tile placeholders.
    - Responses must start like a JPEG, PNG or WebP image. A body that does not, such as
      an HTML error page, is an InvalidPayload error that quotes the start of the body.
  - *Fetching:* `DataClient::fetch(provider, TileRequest)` shares the key handling, rate
    limits, quota, ledger and disk cache of bounding-box requests. The cache key is the
    tile URL without the key.
    - Imagery is streamed only. `estimateDownload()` and bounding-box requests refuse
      imagery providers, and launching a mission downloads no imagery.
  - *Source:* `terrain::OnlineImagerySource` builds the ortho tiles of step 7 from service
    tiles; it shares the abstract `OrthoImagery` with `OrthophotoSource`.
    - Each ortho level uses the zoom whose pixels are just finer than its texels, at the
      origin's latitude, clamped to the layer's range. At 40° N, the 0.25 m level-0 texels
      use zoom 19.
    - The 17 × 17 lattice maps texels to service pixels: world → geodetic → Web-Mercator.
      Samples are bilinear, 2 × 2 per texel when the service pixels are more than 1.25 ×
      finer.
    - An ortho tile needing more than 36 service tiles is left empty, so that coarse levels
      never flood the service.
    - Service tiles are decoded with GDAL from memory (JPEG, PNG including palettes, WebP;
      grey, grey + alpha, RGB, RGBA). A tile of the wrong size is refused with its size.
    - They are looked up in a 96 MB memory LRU, then in the disk cache (stale entries are
      used rather than waiting), then fetched, with at most 64 requests pending.
    - 404, 410 and 204 answers, and invalid images, mark the tile as missing. Missing tiles
      are transparent, so the materials show. Other failures are retried after 60 s, and the
      last one is shown in the Terrain tab.
  - *Incomplete tiles:* while service tiles are on their way, a build returns what it has
    with `complete = false`.
    - `OrthoStreamer` rebuilds such tiles when the source's revision moves (a service tile
      arrived) or after 15 s. Meanwhile a coarser tile stands in.
    - A rebuilt tile carries a new serial, and `OrthoTextures` uploads it into the layer it
      already has.
  - *Application:*
    - The Terrain tab's *Imagery* group (formerly *Orthophotos*) chooses *Orthophoto files*
      or an imagery provider, and its layer. The information line shows the protocol, zooms,
      resolution, tile counts and the last problem.
    - The choice is saved with the scene and presets (`ortho.provider`, `ortho.dataset`).
    - An unknown provider, a missing API key or a provider with no imagery at the origin
      disables or cautions the imagery only. The terrain is still built, and the status bar
      says why.
    - Attribution is "elevation · provider; layer".
    - Settings › Data providers edits imagery providers: tile protocols, zoom ranges per
      layer, tile size. *Test connection* fetches one tile at zoom 15 (within the layer's
      range) and decodes it.
  - *Missions:* `imageryProvider` and `imageryDataset` for online terrain (the key was
    reserved before) and for file terrain, where they are an alternative to
    `orthophotoFiles`.
    - An unknown provider makes the mission invalid.
    - Online imagery is not part of `terrain_data_hash`, like orthophoto files.
  - *Presets* (`assets/providers/`):
    - IGN PNOA (WMTS `GoogleMapsCompatible`, zoom ≤ 20, CC BY 4.0);
    - swisstopo SWISSIMAGE (REST WMTS in EPSG:3857, zoom ≤ 20);
    - USGS The National Map *Imagery Only* (ArcGIS tile cache, zoom ≤ 16, public domain).
    - Their request formats were checked against the live services on 2026-11-21. Their
      licences and fair-use terms come from the providers' published terms and **must be
      rechecked before release** (§12.4.1). No preset needs a key.
  - *Deviations:*
    - Only Web-Mercator grids are supported. WMTS servers with other tile matrix sets
      (such as national grids) must offer `GoogleMapsCompatible` or an EPSG:3857 WMS.
      GetCapabilities is not read: the template gives the request.
    - No bulk download or pinning of imagery for offline use. What was flown stays in the
      disk cache under the provider's `maxAgeDays`.
  - *Tests (mock tile server, `tests/support/imagery_fixtures.hpp`):*
    - Unit: provider validation, URLs for the four protocols, image signatures, tile
      fetching through the cache, the zoom choice, pixel placement within about 1 px, 404
      coverage edges, wrong-size tiles, offline use of the cache, and the streamer
      rebuilding incomplete tiles.
    - Configuration: mission keys and the scene JSON round trip.
    - Application: the Terrain tab with an imagery provider, a mission streaming imagery,
      a missing key disabling only the imagery, an unknown provider making the mission
      invalid, the provider editor and *Test connection*.
- **Step 9 — Built-in file-terrain missions, tests and `docs/TERRAIN.md` (done):**
  - *Missions:* two more real-terrain missions, each opened by a star on *Hover*:
    - *Oeschinensee* (`real-02-oeschinensee`): eight swissALTI3D 2 m GeoTIFF tiles (LV95,
      LN02), imagery from the `swisstopo-swissimage` provider; lake, falls and alp
      waypoints, then a landing.
    - *Sau reservoir* (`real-03-sau`): a Copernicus GLO-30 COG (WGS 84, EGM2008) and a PNOA
      orthophoto (JPEG with a world file, EPSG:25831); a pier and a cliff plateau, then a
      landing.
    - Both waive line of sight: the tours go out of sight of the take-off point.
  - *Data not shipped:* the files (about 33 MB) are fetched into the geodata library by
    `scripts/data/fetch_mission_data.py` (Python standard library only), installed to
    `share/scripts/`. Its manifest `mission_data.json` gives every file's URL, size and
    SHA-256, with the publisher, licence and terms.
    - Downloads go through `.part` files and are checked for size, SHA-256 and (for WMS)
      content type and image signature. The script writes the orthophoto's world file and
      an `ATTRIBUTION.txt`, and asks before downloading.
    - Until the data is there, the missions are *unavailable* with a message naming the
      script. Missing library files are summed up in one message.
  - *No lidar mission:* a swissSURFACE3D lidar tour was dropped: its single tile is
    262 MB for 1 km², too much to ask for one mission.
  - *Fix:* mission item positions given as latitude and longitude are now placed in the
    local frame at the origin's AMSL altitude, like the scene. At height 0 they were
    about 0.2 m off at 800 m from an origin at 1600 m.
  - *Tests:*
    - The manifest and the missions agree on the files, and every file has a licence, an
      https URL and a hash.
    - Without the data the missions say how to get it. With stand-in files of the same
      names, formats and CRSs, they are launched offline, and the spawn and items sit at
      their latitudes, longitudes and heights above the ground.
    - With the real data (`DS_MISSION_LIBRARY=<library>`, skipped otherwise): pads are
      level and beacons clear of the ground, and a terrain-following stick pilot flies each
      tour to success with no collision and no rule warning.
    - The fetch script against a local HTTP server: checks, world files, notices, mission
      selection and refusals.
  - *Documentation:* `docs/TERRAIN.md`.

**M5 implementation notes** (done; deliberate scope decisions):

- **Step 1 — Core items and missions (done):** `core/items`: item types and shapes, the
  registry with 64-bit generational ids, CRUD, change notifications, undo/redo (generations of
  removed items are never reissued, also across undo), a uniform-grid spatial index (radius,
  box, point-in-zone, ray casts against solid items), terrain placement for AGL/AMSL items and
  trigger detection (direction-aware gates tested along the whole tick segment, zone
  enter/exit, waypoint reach). `core/missions`: the mission model, loader and validator
  (schema, then semantic checks: references and their item types, dependency cycles, vehicle
  type, profiles, providers and presets, item properties per type, extent, no-fly zone /
  geofence vs. spawn, reachable thresholds, weather plausibility, time zone, file-terrain path
  rules; missing elevation files make a mission *unavailable*, not invalid), placement of
  items in the world, the flight-rules monitor (also usable in free mode), the mission runner
  (lifecycle, all objective types including survey and inspect, failures, failure injection
  events, scoring and stars) and the catalogue (folder scan, duplicate ids, import, campaign
  unlocks). `core/storage`: `MissionStore` (index sync that keeps progress of missing files,
  attempts, best attempt, progress view). `core/common`: a validator for the JSON Schema
  subset used by the project (unsupported keywords make the schema itself invalid, so it never
  silently checks less than it says). `drone_simulator --validate-mission <file>` (FR-MIS-16)
  runs without a display. `docs/MISSIONS.md` holds the guide and a field reference generated
  from the schema by `scripts/docs/gen_mission_reference.py`.
- **Schema validation is in-house** rather than a third-party JSON Schema library: the subset
  needed is small, and it keeps the dependency list unchanged.
- **Local coordinates** use the engine frame (`z` = south), like the examples; polygon outlines
  are `[x, z]` offsets from the item position, rotated by its heading.
- **Pick-up and drop** need no extra input: the vehicle stays in the zone, low (≤ 1 m AGL) and
  slow (≤ 0.5 m/s), for 1 s. **Landing** counts after the first touchdown following flight,
  once the vehicle has settled (< 0.3 m/s for 1 s); the touchdown speed is the descent rate
  just before contact. **Return home** needs the vehicle to have left the home radius first.
- **Stars:** success gives the first star; `scoring.stars` holds at most two thresholds, for
  the second and third star. Score formulas are documented in `docs/MISSIONS.md`.
- **Default mission (FR-MIS-01b, done):** `assets/default-missions/first-flight.mission.json`
  is copied into the user's mission folder at start-up if it is missing; the built-in and user
  folders are then scanned and the mission index in the database is synchronised (a file
  whose id is already taken keeps no row).
- **Missions tab (list, done):** missions grouped by campaign chapter (*Free missions* for
  missions without a chapter, *Invalid missions* last), with columns *Mission*, *Type*
  (aerial / ground — text instead of an icon for now), *Stars* (best, ★★☆), *Best* (fastest
  success, `m:ss.s`) and *Status* (*Available*, *Locked* with the unlock condition in the
  tooltip, *Unavailable* with the reason, *Invalid*); non-available rows are greyed. The
  details pane below shows the briefing, objectives (numbered when sequential), conditions
  (terrain, local start time, weather, vehicle), rules, star thresholds, the user's results,
  warnings — or every error of an invalid file — and the file path. *Reload* (also
  *Missions › Reload Missions*) rescans the folders (FR-MIS-01a); *Open Folder* opens the
  user's mission folder in the file manager. The first available mission is selected and the
  selection survives a reload, and the last selected mission comes back at the next start
  (FR-UI-03). *Start* (or a double click) opens the briefing. The tab reads everything
  through `MissionsController` (FR-UI-06).
- **Mission flow (FR-UI-07, done):** *Start* → **briefing** (top-down map with shaded terrain,
  items, take-off point and heading, pilot, scale bar and north; briefing text, objectives,
  conditions, rules, stars; the day/night situation of FR-MIS-06a with sunrise, sunset, sun
  elevation, the Moon's phase and a night warning; the vehicle profile when the mission allows
  a change; a warning when the mission needs a controller and none is set up) → *Fly*: the
  mission's terrain and environment replace the free scene without overwriting it, the panel
  hides (FR-UI-04), the vehicle waits on the spawn point in PreFlight and the clock starts on
  arming → the attempt ends (success, failure or *Missions › Abort Mission*), is **stored at
  once** in the user's database (FR-MIS-12), and after 1.5 s the simulation pauses on the
  **debrief** (outcome, stars, per-objective results, score breakdown, penalties, the flight
  path over the map, the comparison with the personal best before this attempt) with *Retry*
  (a clean attempt from PreFlight), *Next mission* (the next available one in list order) and
  *Back to free flight* (the free scene, vehicle and camera come back as they were).
  *Missions › Import Mission… / Import Mission Folder…* copy into the user's folder (asking
  before replacing), reload and select the mission.
- **Mission rules in the simulation:** time scale fixed at 1×, no Free camera, no reset or
  moved home, the vehicle profile locked (unless the mission allows choosing it in the
  briefing), keyboard input refused when `allowKeyboard` is false, the Terrain, Environment
  and Vehicle tabs disabled. Payloads attach their mass (and drag) to the flight model and
  are released at the drop zone. **Solid items and gate frames are obstacles** for the
  flight model: every collision point (propeller tips, landing gear, top) is tested against
  them; contact pushes the drone out and removes its velocity into the obstacle; a hit above
  half the crash speed — or any hit above 1 m/s with spinning propellers — is a crash
  (*"collision with Mast"*), which fails a mission. Gear contacts on top of an obstacle count
  as standing on the ground.
- **HUD (FR-GFX-14):** a mission panel (title, state, timer with the time limit — caution
  below 30 s, warning below 10 s —, one line per objective with its detail and the distance
  to its target, pre-flight problems, cargo) and guidance markers on the current targets
  (diamond on screen, arrow at the edge when off screen), hidden when `rules.showMarkers` is
  false. Rule warnings (height, distance, line of sight, no-fly zones, …) appear with the
  other warnings, in missions and in free flight.
- **Item rendering:** pads, gates (frame, posts down to the ground, direction arrow),
  obstacles (box, building, cylinder, power line, wind turbine; house, tree, cow and man
  drawn from the solid parts they collide with), payloads, inspection
  targets, the pilot, and translucent no-fly zones, geofences, pickup/drop zones and survey
  areas; the current targets glow.
- **Free-mode flight rules (FR-MIS-06d):** *Settings › Flight rules › End the flight when a
  rule is broken* (off by default). The rules are always monitored in free flight and shown as
  warnings; when the setting is on, a broken rule pauses the flight.
- **Items tab (done):** the free scene's items — list, add-item palette (gate, waypoint,
  landing pad, box, pole, tree, building, inspection target, pickup/drop zone, no-fly zone;
  placed 15 m in front of the drone), property editor (name, east/north or latitude/longitude,
  altitude AGL/AMSL, heading, shape, radius, width, length, height, enabled), snap to terrain,
  duplicate, delete, undo/redo and the gate order. The items are drawn and solid in free
  flight, follow the terrain when it changes (AGL) and are saved with the scene in the user's
  settings (`scene.items`). During a mission the tab lists the mission's items, read only.
- **Built-in mission pack (done):** 12 playable aerial missions in `assets/missions/` in three
  chapters — *Basics* (hover, square circuit, precision landing on a hillside, crosswind
  transit), *Racing* (gate run, forest slalom with strict order) and *Operations* (parcel
  delivery, mast inspection, field survey, restricted airspace, dusk patrol with lights and
  VLOS, alpine valley relay) — covering every aerial objective type, plus *Montserrat ridge*
  on online terrain, listed as unavailable until the online elevation source (M4) is built.
  Each sits on real-world coordinates and site elevations; a test checks that pads are on
  level ground, beacons and gates above the terrain and everything inside the terrain area.
- **Deferred:** the 4 ground missions (M6) and the real-terrain missions (GDAL/online sources
  of M4); physical effects of scheduled failures (done in M7 step 6); mouse picking of items in the viewport
  (FR-ITM-06, *S*); a gate-course timer in free flight; the mission editor (FR-MIS-14, *C*);
  the History tab (done in M7 step 7).
- **Tests:** headless missions flown by a stick autopilot through the real world, flight
  model, controller and runner (*First flight* to three stars, the *Gate run* through every
  gate in its arrow direction, cargo mass during a delivery, height-limit failure, crash
  into the mast, crash and abort, retry, free-mode rules with and without enforcement, solid
  free-flight items); the mission pack's validity and placement; GUI tests of the briefing,
  launch, abort, stored attempts, debrief choices, the Retry menu entry, import, the
  free-rules setting and the Items tab; a GL test rendering a mission in flight.

**M6 implementation notes** (done; deliberate scope decisions):

- **Step 1 — Ground vehicle core (done):** the `Vehicle` interface now carries everything the
  loop, missions and statistics need (armed state, battery, mass, camera, sensors, obstacle
  contacts, payload), and `sim::World` runs either type from a `VehicleProfile`
  (`std::variant` of the aerial and ground profiles; `World::aerialVehicle()` /
  `groundVehicle()` for type-specific access). `core/physics` gains the ground profile (JSON in
  `assets/vehicles/ground/`, editable parameter table, derived figures of FR-GND-13), the
  surface materials, the drive controller and `GroundVehicle`: per-wheel spring–damper
  suspension against a per-wheel terrain plane, Magic Formula tyres on the combined slip with
  the surface's grip and rolling resistance, one DC gear motor per wheel on the shared battery
  model, dynamic and holding brakes, static friction when parked, wheel encoders, rollover,
  stuck and water detection, body and wheel contacts with obstacles. The world publishes
  `RolloverEvent`, `VehicleStuckEvent` and `WaterEnteredEvent` and feeds the mission runner's
  ground fields (rolled over, stuck time, in water). The terrain lookup reports the material
  drawn on the terrain (rock → paved, grass, sand, snow). Reference profile
  **`rover-4wd-3kg`**. Model and checks: `docs/GROUND_MODEL.md`.
- **Layouts:** 4- and 6-wheel skid steering (FR-GND-01 M, and 6WD of the S item without
  rocker-bogie geometry). Ackermann crawlers and tracked rovers, and the 6WD and crawler
  reference profiles (S), come later.
- **Drive modes:** Manual (arcade mix) and Speed control. Tank-style sticks are an input
  mapping (`t = (L + R)/2`, `s = (L − R)/2`). The transmitter's mode index selects Manual for 0
  and 1 (the default) and Speed control for 2 and 3; Heading hold (S), traction control (S),
  regenerative braking (S) and the waypoint autopilot (C) are not in this step.
- **Pilot controls:** `ControlInputs` and `PilotCommand` gain `drive` (−1 … 1), `steering` and
  `brake`.
- **Step 2 — Driving the rover in the app (done):**
  - *Keyboard:* `W/S` (or up/down) drive forward and reverse and spring back to neutral like
    the trigger of an RC car transmitter (full deflection in 0.33 s); `A/D` (or left/right)
    steer to ±0.6; `Space` brakes while held; `Enter` arms or disarms either vehicle type
    (`Space` keeps arming aerial drones). `1`–`2` select Manual, `3`–`4` Speed control.
  - *Joystick mappings per vehicle type (FR-IN-03a):* an `InputMapping` carries its vehicle
    type (`vehicleType` in its JSON; older mappings without it are aerial) and stores the
    four channel slots as drive (self-centring stick), steering, forward trigger and reverse
    trigger; the triggers are added to and subtracted from the drive stick. A `brake` switch
    function brakes while its button is held. Presets: gamepad with the triggers (right
    forward, left reverse, left stick steers) or with the sticks (left stick drive, right
    stick steering), both with A = brake, start = arm, right shoulder = Manual/Speed control,
    Y = lights; and RC transmitter AETR/TAER (aileron steers, elevator drives, arm on axis 4,
    mode on axis 5). The calibration wizard covers ground mappings since step 6.
  - *Vehicle tab:* type selector (aerial / ground); the profile list, parameter editor
    (`groundParameters()`) and computed figures (FR-GND-13: top speed, 0 → 90 % time, stall
    torque, static rollover angles, range and endurance, steepest slope per material) follow
    the type. The type is locked during a mission.
  - *Simulation adapter:* the flown profile is a `VehicleProfile`; built-in ground profiles
    load from `assets/vehicles/ground/` and are synchronised into `vehicle_profiles` with
    `vehicle_type = 'ground'`; user profiles of both types are saved there. Settings remember
    the vehicle type (`sim.vehicleType`), one profile per type (`sim.vehicleProfile`,
    `sim.groundProfile`) and one joystick mapping per type (`input.activeProfile`,
    `input.activeGroundProfile`). Sessions are recorded with their vehicle type. Missions
    check that the profile matches the mission's vehicle type, and the briefing offers only
    profiles of that type (ground missions stay unplayable until their step).
  - *Interim visuals:* a procedural rover model (`graphics::RoverModel`: body, wheels that
    turn with the wheel angles and follow the suspension compression, emissive headlights
    and tail lights that brighten when braking); the chase distance is 4 body lengths. The
    HUD shows the drive mode and the drive command (or BRAKE); the Simulation tab shows
    drive/steering bars and ground telemetry (speed and odometry, pitch, roll, slope and
    surface, wheel slip, motor commands).
- **Step 3 — Ground HUD and headlights (done):**
  - *Ground HUD (FR-GFX-20):* for ground drones the altitude column is replaced by `PITCH`,
    `ROLL` and `SLOPE` (degrees and surface material), followed by a load-transfer bar
    ((N_right − N_left) / ΣN; amber zone from 0.6, red from 0.85, where the inner wheels are
    about to lift) and a top view of the wheels coloured by their combined slip (green, amber
    from 0.2 past the tyre's peak grip, red from 0.5 when spinning or locked, grey off the
    ground). The right column shows the forward speed (`R` prefix when reversing), the
    largest slip (`SLIP 8%`), the wheel odometer (`ODO 123 m`), home and pilot distances and
    the wind. Heading, battery and the drive mode stay where they are for aerial drones; in
    Speed control the mode shows the cruise set speed. Warnings: `ROLLOVER RISK` (amber/red
    with the load transfer), `ROLLED OVER — PRESS R TO RESET` (`ROLLED OVER` in a mission),
    `IN WATER` or the water depth, `STUCK` after 2 s without progress, `RC LINK LOST —
    STOPPED` and `BATTERY CRITICAL`; the 120 m AGL rule is aerial-only.
  - *Local lights (FR-GFX-08d):* `graphics::SpotLight` (position, beam axis, colour,
    intensity in candela, inner/outer cone half-angles, range) — the renderer takes up to 8
    per frame (`FrameInput::lights`, std140 UBO binding 3) and adds their illuminance
    (inverse-square, Lambert, smooth cone edge, windowed to 0 at the range) to the terrain,
    objects and the water body. The rover's two headlights are 400 cd LED beams (≈ 16 lux at
    5 m) with 10°/25° half-angles aimed 4° down, warm white (≈ 5000 K), switched with `L`;
    they light the scene from every camera, FPV included. `graphics::spotIlluminance()` is the
    CPU mirror used by the tests. Spot lights do not cast shadows yet.
- **Step 4 — Ramps and bridges as driving surfaces (done, FR-ITM-13 S, FR-GND-03):**
  - *Shapes:* a new solid part, the wedge (`SolidPart::Kind::Wedge`), rises linearly
    towards local −z (or +z) with a vertical high end. A `ramp` is one wooden wedge,
    `sizeM` [w, h, l] (1 × 0.25 × 1.5 m) rising towards its heading. A `bridge` is a 0.3 m
    thick concrete deck whose top is `h` above its position over `l` (1 × 0.5 × 3 m), approach
    ramps of `rampLengthM` (default a 12° slope) at both ends and yellow kerbs along the deck
    (`kerbs: false` removes them); a deck higher than its thickness leaves room to pass under.
    Both stay solid for aerial drones (and block the line of sight).
  - *Drive surfaces:* `items::driveSurfaceAt()` gives the highest top of a ramp or bridge at
    a horizontal position that is not above a given height. `World::surfaceAt(point)` takes
    the highest of the terrain and those tops (paved); the ground drone samples it for every
    wheel (at the hub at full extension) and body point through the new
    `EnvironmentSample::surfaceQuery`, and the environment, snapshot and HUD use it under the
    vehicle (slope, surface, water depth — a rover on a bridge over water stays dry; an
    aerial drone lands on a deck).
  - *Editing and missions:* the Items tab palette has *Ramp* and *Bridge* (width, height,
    length); the mission schema and validator accept `rampLengthM` and `kerbs` on bridges.
    Items are drawn as they collide (wedge faces wound outwards).
- **Step 5 — Ground missions and the parking zone (done, FR-MIS-03a/04a, FR-ITM-13 M):**
  - *Playable:* ground missions are no longer blocked (`unplayableReason()` only rejects
    online and file terrain, M4). The runner already scored the ground objectives (2D
    waypoints, deliveries "stopped" in the zones, `park` with position, heading, speed and
    hold time, `maxStuckS`, rollover and `maxCollisionSpeedMs` failures).
  - *Parking zone:* drawn always as a bay painted on the ground (white lines scaled to the bay,
    yellow heading mark, 5 cm above the ground), placeable from the Items tab (*Parking zone*,
    width × length, default 0.7 × 0.9 m for the rover).
  - *Missions:* a `ground` chapter of four built-in missions for the rover-4wd-3kg — *First
    drive*, *Tight parking*, *Sample run at dusk* (headlights required) and *Bridge course*
    (gates on the ground and on a bridge deck, a kicker ramp). The pack test checks that
    ground gates open at the driving surface (terrain or deck) and that bays, ramps and
    bridges stand on level ground; a scripted driver on the drive and steering controls
    completes each mission in the simulation (`MissionDrive.*`).
- **Step 6 — Calibration wizard for ground mappings (done, FR-IN-02/03a M):** with a ground
  mapping the wizard asks for centre, full travel, *drive forward*, *reverse* and *steer
  right*. The forward move tells a drive stick or pistol-grip trigger (self-centring: drive
  slot, reverse = the same axis pulled back) from a gamepad trigger (an axis resting at one end
  of its travel: forward slot, then the other trigger for reverse); the preset's unused slots
  are cleared, switches, deadzones and expo kept. Steering on a trigger is refused. The
  Controls tab enables *Calibrate…* for both types (ground needs 2 axes) and names its button
  *Drive With This* for ground mappings.
- **Step 7 — Model files for vehicles and items (done, FR-GFX-12 M, deferred here from M3):**
  - *Loader:* `graphics::loadMesh()` reads glTF 2.0 (`.gltf`, `.glb`) and Wavefront OBJ
    (`.obj` + `.mtl`) with Assimp (Debian `libassimp-dev`): triangles only, node transforms
    baked in, normals generated where missing (smoothed across bends under 60°). Materials
    give the colour only — base colour (OBJ: diffuse) × vertex colours, opacity (translucent
    meshes are drawn blended) and emission (shown in the emitted colour, 300 cd/m² at factor 1,
    times `KHR_materials_emissive_strength`); **texture maps are ignored**. At most 2 million
    vertices. Models use the glTF axes (metres, +Y up, front towards +Z).
  - *Placement:* the mesh is turned into the vehicle body frame or the item frame and fitted
    — scaled uniformly to fit the procedural body (vehicles) or the item's shape (obstacles),
    centred, resting on its bottom — so any model can be used without editing its units or
    origin. Wheels, propellers and lights stay procedural (they move); collision shapes do not
    change.
  - *Where models are named:* `modelFile` in a vehicle profile (Vehicle tab › *Model:*
    *Choose…* / *Built-in*, then *Apply*; an absolute path, or a name looked for in
    `~/.drone_simulation/models/` and then `<share>/models/`) and the `model` property of
    obstacle items in missions (looked for first in the mission's folder; no `..`). A file that
    cannot be read is reported (status bar and log) and the procedural model is drawn.
  - *Built-in:* `models/crate.obj`, a 1 m wooden crate (original, MIT).
- **M6 is complete.** Left for later, all S/C priority: shadows of the headlights and lights on
  items (FR-GFX-08d); 6WD rocker-bogie, Ackermann crawler and tracked layouts and their
  reference profiles (FR-GND-01/12); Heading hold, traction control, regenerative braking (S)
  and the waypoint autopilot (C) (FR-GND-05/11); texture maps on model files; choosing item
  models in the Items tab.

**M7 implementation notes** (done except step 9, which moved after M4; deliberate scope
decisions):

- **Plan:** (1) weather generator core; (2) weather effects on the vehicles (rain, wet and
  snowy ground, flying into cloud) and the battery checks of NFR-REAL-11; (3) weather
  rendering (cloud layers, overcast light, rain and snow, ground fog); (4) the generator in the
  Environment tab and the HUD (FR-WX-12); (5) Position hold and return to home; (6) failure
  injection; (7) telemetry recording, replays and the History tab; (8) NFR-REAL validation;
  (9, S) the online orthophoto provider, which needs the raster sources of M4.
- **Step 1 — Weather generator core (done, FR-WX-01…03, FR-WX-10 M):**
  - *`core/weather`:* `WeatherSettings` (seed, preset, overrides, timeline, `forecast`; the
    JSON form is the mission's `environment.weather`), the 13 presets as data
    (`assets/environment/weather_presets.json`, `WeatherPresetTable`; a preset sets only the
    parameters it lists — snow presets also a temperature below 0 °C), and
    `WeatherGenerator`. The conditions start from 15 °C, 1013.25 hPa, 50 %, calm, clear and
    50 km, then the preset, then the overrides (a dew point gives the humidity, Magnus
    formula). Each timeline entry blends with a smoothstep over `transitionS` into the
    previous target changed by its preset and overrides: numbers linearly, visibility in log
    scale, the wind direction along the shorter arc, cloud layers pairwise (a layer missing on
    one side fades in or out), types and flags at half-way. Wind levels may be fractional
    while blending (`WindLevelTable::interpolated`).
  - *Fluctuations (seeded value noise):* partial cloud cover ±0.75 okta over ~5 min (none at
    0 or 8 oktas), precipitation rate ±40 % over ~2 min with the visibility ∝ rate^−0.6.
    Cloud layers drift with the mean wind at mid-layer height (log profile, constant above
    300 m); `advance()` keeps the clock and the drift, `seek()` jumps.
  - *In the world:* `sim::World` runs the generator (its clock restarts with every reset, so
    a mission's timeline counts from the spawn); the air (density, barometer QNH, battery
    ambient temperature), the wind level and direction (changed in place with
    `WindModel::setParameters`, without restarting turbulence or gusts) and the visibility
    come from it. The visibility used for VLOS is taken at the vehicle's height: 60 m inside
    a cloud layer of ≥ 4 oktas, the clear-air value above a ground fog layer. Free mode keeps
    the M3 Environment tab: its air, wind and visibility become a clear-sky setting
    (`WeatherSettings::fromBasic`; replaced by the full generator in step 4). The snapshot
    carries the conditions and cloud drift for the rendering and the HUD.
  - *Missions:* the loader turns `environment.weather` into `WeatherSettings`; the mission
    scene shows its start conditions in the Environment tab; the briefing shows the summary of
    FR-WX-10 (`weather::describe`) and, with `forecast: true`, one line per timeline entry.
- **Step 2 — Weather effects on the vehicles (done, FR-WX-06/07/08 M, NFR-REAL-11 battery):**
  - *Ground state:* the generator keeps a uniform `GroundCover` (wetness 0–1 and snow depth):
    soaked by 1 mm of rain, drying with 30 min at 50 % humidity (slower when humid, not while
    raining), snow settling at ≤ 1 °C (10:1) and melting at 5 mm/(°C·h); a session that starts
    in rain or snow starts on wet or snow-covered ground. Spatial variation (FR-WX-04, S) is
    not modelled.
  - *Ground drones:* `surfaceProperties(material, cover)` blends each material towards its wet
    grip and rolling resistance (wet asphalt μ 0.6, dirt to mud, firmer wet sand, slush) and
    snow ≥ 2 cm turns any surface to snow with extra rolling resistance; the HUD and Simulation
    tab say `wet grass` or `snow`. Water ingress above an IP rating (S) is left for the failure
    step.
  - *Aerial drones:* a rain film (≤ 0.1 mm over twice the drag areas, τ 60 s wetting / 300 s
    drying) adds mass at the CoG; the drops' momentum flux acts on the drag areas (well under
    1 % of the weight in heavy rain). Motor failures from rain above the IP rating (S) and
    icing (C) are left for later.
  - *Battery (FR-WX-08, NFR-REAL-11):* the polarisation branch now has its own, steeper
    temperature law (Ea/R 6000 K LiPo, 5000 K Li-ion), so the voltage sags in the cold as on
    the datasheet curves; a test discharges cells at 0.5 C from −20 to 40 °C against
    representative curves (`tests/fixtures/battery/temperature_curves.csv`, not taken from one
    product: an open item to check against a real datasheet).
  - *Found while testing:* the accelerometer of the simulated multirotors sees the tilt only
    through drag, and the reference profiles' rotor drag (≈ 0.04 s⁻¹) is weaker than measured
    on real quadrotors (0.1–0.6 s⁻¹). After a hard bank in Angle mode without a GPS fix the
    true attitude drifts by ≈ 0.5°/s with the estimate level. The Angle-mode test now checks
    the self-levelling 1 s after centring; the drift is left for the NFR-REAL step (8), since
    changing the rotor drag moves every validated flight figure. (Resolved in step 8.)
- **Step 3 — Weather rendering (done, FR-WX-09 M and the S items marked, FR-GFX-09):**
  - *Data path:* `graphics::makeWeatherView()` turns the snapshot's `WeatherConditions`, cloud
    drift and `GroundCover` into a `WeatherView` (`FrameInput::weather`), with heights measured
    from the ground at the take-off point; `assets/shaders/weather.glsl` holds the shared
    shader code (uniform block 4).
  - *Clouds (M):* each layer is a slab between its base and top whose horizontal density is
    a 5-octave value noise moved by the layer's drift, thresholded so that it covers
    oktas / 8 of the sky (`cloudCoverThreshold()`, checked against the noise's distribution
    in a unit test). Cumulus and cumulonimbus have flat bases and narrow tops, cirrus are
    streaked along the wind. Rays are marched through the slabs (12 samples per layer for the
    sky, 3 for surfaces seen through or above clouds) with interleaved-gradient jitter; the
    extinction (0.04–0.08 /m, ≈ 60 m inside a cloud; cirrus 0.002 /m) hides what is behind.
    Light: multiple scattering from the light on the top falling to the two-stream diffuse
    transmittance at the base (dark bases under thick clouds), single scattering of the direct
    beam with a Henyey–Greenstein phase (bright thin edges), sun and moon alike, so the moon
    lights the clouds at night and the clouds hide the moon, the stars and the airglow.
    Distant clouds fade into the haze (Koschmieder); beyond 150 km, and below the horizon,
    a layer is drawn as a continuous band. Ray-marched volumetric clouds (S) are not done.
  - *Light under the clouds (M), cloud shadows (S):* the direct sun and moon light is
    multiplied by the transmittance of the clouds towards them at each point (moving cloud
    shadows on the terrain, water glints and highlights); the diffuse light is the clear-sky
    light dimmed by the layers above plus the direct light they scatter down
    (`skyFactor`, `beamDiffuse`). An overcast therefore lights the scene with 10–30 % of clear
    daylight and no shadows (the shadow maps still run but carry no light); the haze and the
    reflections under it take the overcast's grey.
  - *Fog and visibility (M, FR-GFX-09):* a ground fog is a layer up to its top with the fog's
    visibility under air of 30 km visibility; a visibility under 20 km without fog (haze,
    rain, snow, and the Environment tab's visibility in free mode) becomes a layer up to the
    lowest cloud base (1500 m without clouds) under 20 km air. The layer scatters the grey
    light of its surroundings (dim and moonlit at night), so the sky above the clouds keeps
    full sunlight instead of a reddened sun.
  - *Precipitation (M):* rain streaks and snowflakes in a 24 m cube around the camera (600–
    8000 drops, 800–10000 flakes by rate), fixed in the world and moved by the fall speed and
    the wind; streaks follow the drops' velocity relative to the camera over 1/30 s; they are
    lit by the surroundings and by the headlights. Sleet mixes both.
  - *Ground (S):* wet ground is darker and reflects the sky with a Fresnel sheen; lying snow
    from 3 mm (full at 4 cm) whitens slopes up to about 35°, fading out by 55°.
  - *Raindrops on the FPV lens (S):* in rain, drizzle or sleet the FPV lens gets wet (τ 8 s;
    up to fully wet from 4 mm/h) and dries in about a minute after; the tone-mapping pass then
    draws out-of-focus drops on a grid of cells that appear, run down a little and vanish,
    each showing a small upside-down image of the scene with a darker rim.
  - *Lightning (C):* in a thunderstorm about two flashes a minute (seeded by the weather
    clock, one to three strokes of tens of milliseconds) add 0.3–10 lux of diffuse light
    that lights the clouds from within and the ground: invisible by day, a bright flash at
    night. No bolts are drawn.
  - *White balance:* the tone mapper adapts (Bradford) a 6000 K white — midday sun plus sky —
    to sRGB's D65, like a camera's daylight preset. Without it, everything lit by daylight
    was slightly warm and pink, most visibly grey overcast; sunsets, headlights and twilight
    keep their colour relative to daylight.
  - *Cost:* the cloud-cover thresholds come from a per-type table (64 steps, built when a
    cloud type first appears); the exact bisection takes milliseconds and the view is rebuilt
    every frame.
  - *Simplifications:* the World's visibility rule (60 m inside a layer of ≥ 4 oktas) does
    not follow the drawn cloud gaps; heights are flat above the take-off ground (terrain
    following and spatial variation, FR-WX-04, are not modelled).
  - *Cost:* 9.9 ms per 1080p frame with broken clouds and heavy rain on an RTX A2000
    (5.9 ms clear), within NFR-PERF-01.
- **Step 4 — The generator in free mode and on the HUD (done, FR-WX-12 M, FR-WX-05 M,
  FR-WX-10 HUD):**
  - *Settings:* `EnvironmentConfig::freeWeather` holds the free-mode generator settings — seed,
    preset, and the clouds, precipitation, fog and thunderstorm as overrides that are always
    set, plus one timeline entry and its on/off switch. The tab's temperature, QNH, humidity,
    wind and visibility complete them (`freeWeatherSettings()`); the whole object is saved in
    `scene.environment.weather` in the mission JSON form (older settings without it start
    clear). Changing any of it calls `World::setWeather`, which rebuilds the generator
    without restarting its clock; air, wind and visibility edits still change only their
    members.
  - *Environment tab:* a **Weather** group with the preset (loading it replaces the sky and
    copies the air, wind and visibility it lists into their fields, so the fields always show
    what flies), the seed with *Randomise*, three cloud layers (type, oktas, base, top; 0 oktas
    = no layer), precipitation type and intensity (the representative rates of FR-WX-01),
    visibility, fog top, thunderstorm, and the timeline: *Change to* a preset after N minutes
    over M minutes. One entry covers free flying; missions keep the full timeline. Below it,
    the FR-WX-10 summary of the start conditions and the plausibility checks.
  - *Plausibility (FR-WX-05):* `weather::checkPlausibility` checks conditions (snow or sleet
    above 4 °C, fog under 80 % humidity, a thunderstorm below wind level 2, a cloud top not
    above its base, a cloud base below the fog top). The tab shows its findings in red
    (impossible) or amber (unusual) without refusing the setting; the mission loader keeps
    its checks on the JSON, now with the shared limits.
  - *HUD (FR-WX-10):* a METAR-style line under the wind: the lowest layer of at least half an
    okta (`FEW`/`SCT`/`BKN`/`OVC` and its base, `CB` for cumulonimbus, `SKC` without one), the
    present weather (`-RA`, `+TSRA`, `SN`, `RASN`, `DZ`, `FG`, `BR`, `HZ`), the visibility at
    the vehicle (60 m inside a cloud) and the temperature, e.g. *WX BKN 900 m -RA VIS 6 km
    12 °C*. Amber under 5 km or with precipitation, red under 1 km or in a thunderstorm.
    Heights in feet, visibility in miles and temperature in °F with imperial units.
- **Step 5 — Position hold and return to home (done, FR-PHY-13 M/S, FR-PHY-14 failsafe RTH,
  FR-WIND-06):** details in `docs/FLIGHT_MODEL.md` §7.
  - *Position hold:* altitude hold plus a GPS position loop on the estimate. The sticks set a
    horizontal speed (8 m/s at full stick). Released, the drone brakes, then holds the point
    where it stopped, using a velocity PI whose integrator learns the wind. The acceleration
    is limited by the Angle-mode tilt limit, so the drone holds up to its wind capability and
    is blown away beyond it. Without a GPS fix it flies as altitude hold, with a HUD warning.
  - *Return to home:* home is set at arming or at the first fix after it. Stages:
    climb to 30 m above the arming altitude (skipped within 10 m of home) while turning
    towards home → fly back at 8 m/s → land over home and disarm. The switch (input mapping
    or key `H`) works from any mode; switching it off gives control back. The HUD shows
    *RTH climb/return/land*.
  - *Failsafe:* RC loss returns home with RTH when there is a fix and a home, else lands in
    place as before.
  - *Arming:* the RTH switch must be off, and Position hold needs a GPS fix.
  - *Parameters:* six new controller parameters per profile, editable in the Vehicle tab:
    `maxHorizontalSpeedMs`, `positionGain`, `velocityP`, `velocityI`, `rthAltitudeM`,
    `rthSpeedMs`.
  - *Validation:* the controller holds its estimated position within 2 m in winds up to each
    profile's limit (within 1 m in calm air). The true position follows the GPS error: RMS
    1.5 m for the camera and survey drones, 2.3 m for the racer. RTH lands within about 2 m
    of home. No terrain following: the return height is barometric above the take-off
    point, so the pilot must keep it above obstacles on the way.
- **Step 6 — Failure injection (done, FR-PHY-15 S, FR-MIS-07 S):**
  - *Model:* `physics::FailureInjector` holds the active failures, each with its own clock
    (`durationS`, or until reset). Every tick `sim::World` turns them into `VehicleFaults` and
    hands them to the vehicle:
    - sensor errors in `SensorSuite`: GPS without a fix, a GPS offset that grows at `driftMs`
      up to `magnitudeM` in a seeded direction, a compass offset, a baro offset growing the
      same way;
    - the share of thrust each rotor keeps (the ESC output scaled by its square root) or the
      drive voltage of a wheel motor.

    A battery drop takes `fraction` of the rated capacity at once (`Battery::loseCapacity`).
    RC loss clears the link of the controls the vehicle sees.
  - *Behaviour:* the effects come out of the existing models, not scripts:
    - Position hold follows a drifting GPS, and becomes altitude hold without a fix.
    - Altitude hold sinks when the baro reads high.
    - A compass offset turns the estimated heading.
    - With 70 % thrust left on one motor all three quads lurch, then fly on level. A dead
      motor brings a quad down; on the rover it drags its side.
    - RC loss starts the failsafe (RTH with GPS).
  - *Missions:* the runner's `FailureInjected` event becomes `World::injectFailure` with the
    file's parameters (`sim::missionFailure`). The loader rejects a motor above 4 on an
    aerial drone. A reset or a retry ends every failure.
  - *Free mode:* a **Failure injection** group in the Simulation tab (type, motor and
    thrust left, duration, *Inject*, *Clear all*, the active list); disabled during missions.
  - *Events:* `FailureEvent` (started / ended) in the event log. The HUD shows only what the
    pilot would see (GPS no fix, failsafe, the drone's behaviour).
- **Step 7 — Telemetry recording, replays and the History tab (done, FR-X-05 M, FR-X-06 S,
  FR-DB-07, §4.11 History):**
  - *Sessions:* a session runs from one reset of `sim::World` to the next (a mission attempt
    is one session; changing the mission ends one). With recording on, the world keeps for
    the current session:
    - the replay log (`sim::ReplayLog`): a header with everything a reset does not restore —
      seed, reset count (the wind is seeded with it), world time, tick count and clock,
      vehicle profile, weather settings, air, wind, visibility, geographic origin, take-off
      point, time step — plus the application's scene (terrain, environment and items as
      JSON) and the mission id;
    - the controls of every tick (`InputEncoder`: per tick a mask of the changed fields, the
      analog axes on a 16-bit grid; about 1–5 bytes a tick, so 2 h at 500 Hz stay under
      20 MB);
    - every change made from outside between ticks as a command stamped with its tick: time
      step, failure injected or cleared, weather, wind, air, visibility, clock, geographic
      origin, mission retry and abort. Changes the world makes itself (a mission's scheduled
      failures) are not recorded: they happen again on replay;
    - 20 Hz telemetry samples (the `telemetry_samples` columns).

    While recording, the controls are quantised to the stored grid on the live run too, so
    the replay sees exactly what the pilot flew. A terrain or item change before the vehicle
    first arms starts a new session on the new scene (the vehicle is reset in place). After
    that, the session is kept but marked not replayable. Sessions stop recording after
    2 h (`truncated`).
  - *Replay (FR-X-06):* `World::startReplay` puts the world back into the header's state:
    - vehicle, seeds, failure injector, weather, clock;
    - world time and tick count (so the sun and the weather clock match);
    - the reset count less one, then it resets.

    It then ticks with the recorded controls, time steps and commands, ignoring the controls
    and time step the loop passes. At the end it holds the last state. Every reset starts
    from the same sun position, so a reset is a full restart point. Tests replay an aerial
    and a ground session in a different world (another seed, vehicle and history) with a
    weather timeline, injected and cleared failures, wind, air, visibility, clock and
    time-step changes. They check that position, velocity, attitude, energy and statistics
    are bit-identical (FR-X-05). An integration test does the same through the application:
    storage, scene rebuild and controller.
  - *Storage (FR-DB-07):* schema version 2 adds `replays` (session id, mission key,
    replayable, duration, tick count, header/commands JSON, input stream BLOB; deleted with its
    session). `RecordingStore::saveFlight` writes the session row, its telemetry samples, its
    replay and the link from the mission attempt (`mission_attempts.session_id`) in one
    transaction on the storage thread; the simulation controller only collects finished
    sessions (`SimulationLoop::takeRecordings`) and polls the futures, so the GUI never waits.
    The attempt is linked through `SimulationController::linkNextSessionToAttempt` (the
    attempt is saved when it ends, its session at the next reset).
  - *Application:* `SimulationController::startReplay` shows the session's scene (its
    terrain, environment and items, or the mission's scene for a mission attempt, which must
    still be installed) through the scene controller's override used by missions, flies the
    recorded profile at the recorded tick rate and locks the profile, reset, take-off point and
    failure injection. Pause, single step and the time scale work as usual (the scale gives
    fast-forward). `stopReplay` brings the free-flight scene, profile and take-off point back.
    The HUD shows `REPLAY 0:12 / 1:30` under the clocks, then `REPLAY ENDED` in amber.
  - *History tab:* flight sessions (start, vehicle, mission, duration, max AGL, max speed,
    distance, energy, crashes, replay), mission attempts (outcome, stars, score, time and the
    session they link to — selecting one selects its session), and the telemetry of the
    selected session drawn with QPainter (`TelemetryPlot`, no Qt Charts). The plot shows
    altitude, speed, battery voltage or current, throttle, roll and pitch, or the ground track
    from above. Buttons: *Replay*, *Stop replay*, *Delete* (with its telemetry and replay) and
    *Refresh*. Every database request is asynchronous.
  - *Not done:* ghost replay of the personal best next to the live vehicle (FR-MIS-15, C),
    seeking inside a replay (restart it instead), and replaying a session recorded with an
    older replay format (none exists yet).
- **Step 8 — NFR-REAL validation (done, except the M4-dependent items):**
  - *Rotor drag (from step 2):* `rotorDragCoefficient` goes from 0.5 to 1.5 in the three aerial
    profiles, about 0.2–0.3 s⁻¹ of linear drag at hover, as measured on real quadrotors. The
    body drag areas were re-fitted so the top speeds stay within ±10 % of the references
    (30.8 / 16.6 / 18.4 m/s against 30 / 16 / 18). Maximum wind: 30.8 / 15.1 / 16.1 m/s, so the
    maximum wind levels are unchanged (8 / 5 / 5).
  - *Estimator:* the Angle-mode drift without a GPS fix came mostly from motor vibration
    starving the accelerometer correction (trust ≈ 0.2), not from the drag. The fix:
    - the accelerometer is low-passed at 2 Hz in the world frame;
    - the trust window is ±0.1 g (was ±0.05 g);
    - the rest calibration of the gyro is a running mean;
    - in-flight bias learning is 10× slower.

    After a full bank the drone now leans at most 7° while braking and settles level within
    about 25 s.
  - *Checks added:*
    - NFR-REAL-04: Angle-mode self-levelling without a GPS fix.
    - NFR-REAL-08: closed-loop Position hold in a steady wind; tilt and power within ±10 %
      of `windHoldState()`, measured 0.3° and 2.3 %.
    - NFR-REAL-09: braking distance per surface, within ±10 % of `v²/(2μg)`.
    - NFR-REAL-09: tilt-table rollover angle, within ±2° of the rigid-body value with
      near-rigid springs, and of the value corrected for the suspension lean with the
      profile's springs.
    - NFR-REAL-11: Koschmieder fog. The renderer's extinction, fog plus aerosol plus Rayleigh
      at 550 nm, puts 2 % contrast at the weather's visibility. Its Rayleigh share is now the
      green coefficient.

    The figures are in `docs/FLIGHT_MODEL.md` §9–10 (with a status table per requirement)
    and `docs/GROUND_MODEL.md` §8.
  - *Test changes:* the position-hold test now checks "blown away" at the first level whose
    mean wind is 20 % beyond the limit, measured from the take-off spot. Brief gusts above
    the limit only nudge a drone with mostly linear drag. When the thrust runs out, the
    drone sinks into the weaker wind near the ground and holds there.
  - *Not done:*
    - NFR-REAL-05 and NFR-REAL-12 need the M4 GDAL / PDAL work.
    - The battery temperature curves of NFR-REAL-11 are still representative, not taken from
      a datasheet.
- **M7 is complete (steps 1–8).** Step 9, the online orthophoto provider (FR-TER-O10, S), is
  **moved after M4** (done as M4 step 8). It drapes imagery over georeferenced terrain, which procedural terrain
  is not (FR-TER-I03). It also reuses the orthophoto reprojection, mosaic and GPU streaming of
  §4.5.5. Those come from the remaining M4 work, which needs `libgdal-dev` and `libpdal-dev`.
  Also left for later:
  - NFR-REAL-05 and NFR-REAL-12 (M4);
  - a real battery datasheet for NFR-REAL-11;
  - volumetric clouds (S);
  - ghost replay (C) and seeking inside a replay.

---

## 11. Decisions, Assumptions & Open Questions

**Confirmed decisions:**
1. **Goal:** realism, with mission-based play (free flight as sandbox).
2. **Terrain:** random procedural generation, **or** real elevation (DEM) from a web API
   (OpenTopography first), **or** from **elevation files defined in the mission JSON**:
   rasters (**COG** recommended, plus GeoTIFF, `.hgt`, `.asc`, `.img`) or **LAS/LAZ** lidar.
   Orthophotos come from **files defined in the mission JSON**; online imagery later.
3. **Persistence:** Qt SQL module (`QSQLITE`); the rest of Core stays Qt-free.
4. **Per-user database:** only the OS user currently running the application, via
   that user's home directory; no in-app accounts.
5. **Terrain dimensions are optional**, set by the user or by a mission; omitted ⇒
   unbounded streaming.
6. **License:** MIT, Copyright (c) 2026 Juan Maria Gomez Lopez (§12).
7. **Main screen** always shows the drone's **UTM coordinates** and **elevation**.
8. **Wind levels 0–10** affect the flight; levels follow the Beaufort scale (§4.2.1).
9. **Ground drone (UGV):** a terrestrial drone that only drives, never flies; it is an
   **RC-style rover** (§4.2.2).
10. **Missions are described by JSON files** (self-contained, validated by a published JSON Schema, §4.9.1).
11. **Mission date/time matters:** day or night follows from the date and time, and the
    **sun and moon positions must be astronomically correct** (§4.7.1).
12. **Elevation API: OpenTopography first** (Global DEM + USGS 3DEP APIs, user's own key, §4.6).
13. **Weather:** produced by a seeded weather generator described in the mission JSON (§4.2.3).
14. **Binary distribution:** `.deb` package installing to `/opt/drone_simulator` (§7.1).
15. **OpenTopography keys:** non-academic only (50 calls per 24 h).
16. **Lidar in v1.0: DTM only** (bare ground). DSM and buildings/trees as obstacles come later.
17. **UTM display** always uses the zone of the drone's actual position.
18. **Reference vehicles are generic types**, not specific commercial models: 5″ FPV racer,
    sub-250 g camera drone, ~1.5 kg survey quad; RC-style rovers for the ground drone.
19. **Flight rules by default:** missions enforce **120 m AGL** and **visual line of sight**
    unless the mission explicitly overrides them (FR-MIS-06b).
20. **Orthophotos from files** listed in the mission JSON (COG/GeoTIFF; §4.5.5). Elevation
    and orthophoto files share the mission folder and the geodata library
    `$HOME/.drone_simulation/library/`.
21. **VSOP87:** the truncated VSOP87D Earth table is distributed under the project's MIT
    licence, with attribution to Bretagnon & Francou (IMCCE) (§12.4).
22. **BSC5:** the converted Yale Bright Star Catalogue extract is distributed under the
    project's MIT licence, with attribution to Hoffleit & Warren (NASA ADC / CDS) (§12.4).

**Assumptions (please confirm or correct):**
1. **Vehicle types** — aerial: quadcopter (X configuration) for v1.0, hexa/octo later;
   ground: RC-style 4WD skid-steer rover for v1.0; 6WD rocker-bogie and Ackermann
   RC crawler as "should".
2. **RC transmitter support is a must** for a realistic simulator (via SDL3);
   keyboard remains for testing and free flight/drive.
3. **PDAL** (BSD) is acceptable for reading and rasterizing LAS/LAZ (alternative: LASzip
   + our own LAS parser and rasterizer — fewer dependencies, more code to maintain).
   **GDAL** (MIT) is acceptable for raster files and decoding/reprojecting API payloads, and
   **libcurl** for HTTP. Qt Network is not used, to keep Core Qt-free.
4. **"Tab menus"** — a side panel with a `QTabWidget` next to the 3D view (§4.11).
5. **Target platform** — Debian 13 (amd64) first; other Linux distributions next; Windows best-effort.
6. **Elevation shown** on the main screen is **AMSL** in the vertical datum of the
   provider's data, with AGL height next to it.

**Open questions:**
- **Online orthophotos:** *answered in M4 step 8.* Imagery providers are configured like
  elevation providers. The presets are open national services: IGN PNOA, swisstopo
  SWISSIMAGE and USGS. Their licences must be confirmed before release.

---

## 12. Licensing

### 12.1 Project license
The project is released under the **MIT License**:

```
Copyright (c) 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
```

Every source file carries:
<!-- REUSE-IgnoreStart -->
```cpp
// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
```
<!-- REUSE-IgnoreEnd -->

### 12.2 Compatibility analysis

**Conclusion: MIT is possible.** No required dependency forces a copyleft license on
the project's own code. One dependency (Qt, LGPLv3) adds obligations when *binaries* are
distributed, and a few areas need care (below).

| Component | License | Compatible with MIT project? | Conditions / notes |
|-----------|---------|------------------------------|--------------------|
| Qt 6 `Core`, `Gui`, `Widgets`, `OpenGLWidgets`, `Sql`, `Test` (all in qtbase) | LGPLv3 (or GPL / commercial) | **Yes**, with conditions | **Link Qt dynamically.** With the `.deb` (§7.1) Qt is a Debian dependency, not shipped by us, so Debian already meets these obligations. If we ever bundle Qt: ship the Qt license texts, state that Qt is used under LGPLv3, provide (or point to) the Qt source used, and let users replace the Qt libraries (`.so`/`.dll` files are enough). Don't statically link Qt or modify it without publishing those changes. `Qt6::Test` is only used by tests, which aren't shipped. |
| Qt add-on modules: **Qt Charts**, Qt Data Visualization, Qt Quick 3D, Qt Virtual Keyboard… | **GPLv3 only** (open source) | **No** | **Must not be used.** Using them would force the distributed program under GPLv3. |
| SQLite (inside Qt's `QSQLITE` plugin) | Public domain | Yes | None. |
| GDAL + PROJ | MIT / X11 | Yes | Ship license texts. **Caveat:** distro builds of GDAL may link optional copyleft libraries (e.g. Poppler for the PDF driver is GPL). Source-only releases are unaffected. For **bundled binaries**, use a minimal GDAL build (GTiff/COG, SRTM, AAIGrid, HFA drivers; PROJ, libtiff, libgeotiff, zlib, libdeflate/zstd), all permissive. |
| libtiff, libgeotiff, zlib, zstd, libdeflate (via GDAL) | libtiff (BSD-like), MIT/BSD, zlib, BSD | Yes | Ship notices. |
| SDL3 | zlib | Yes | No attribution required in binaries (credit welcome). |
| GLM | MIT (or Happy Bunny) | Yes | Choose MIT; ship notice. |
| glad (generated loader) | Generator MIT; generated code public domain / CC0, Khronos XML parts Apache-2.0 | Yes | Keep the generated header comments; list Apache-2.0 attribution in notices. |
| nlohmann/json | MIT | Yes | Ship notice. |
| json-schema-validator (pboettch) | MIT | Yes | Ship notice. |
| GoogleTest | BSD-3-Clause | Yes | Test only; not distributed. |
| Doxygen, CMake, Ninja, clang-tidy, REUSE tool | GPL / BSD / Apache | Yes | Build tools only; their licenses don't apply to the project or its generated output. |
| PDAL | BSD-3-Clause | Yes | Debian dependency; ship notice if ever bundled. |
| LASzip / laz-perf (via PDAL) | Apache-2.0 | Yes | Not used directly. LAStools/LASlib is **not** used (parts are proprietary or LGPL). |
| libcurl | curl licence (MIT/X-style) | Yes | Ship notice. |
| OpenSSL 3 / GnuTLS (TLS for libcurl) | Apache-2.0 / LGPL-2.1+ | Yes | Use the system library (dynamic). If bundling, OpenSSL 3 is simplest (Apache-2.0); GnuTLS needs LGPL compliance like Qt. |
| QtKeychain | BSD-3-Clause | Yes | Ship notice. |
| cpp-httplib | MIT | Yes | Tests only. |
| Mesa / GPU drivers / OpenGL | System components | Yes | Not distributed. |

### 12.3 Code provenance rules
- **No code copied from GPL firmware or simulators**, e.g. Betaflight, INAV, ArduPilot
  (GPLv3), Gazebo plugins under GPL. Re-implementing published *math* (PID, rate/expo
  curves, mixer equations) from documentation and papers is fine.
- Permissive sources may be reused **with attribution** in `THIRD_PARTY_NOTICES.md` and
  the file header: e.g. PX4 (BSD-3-Clause), FastNoiseLite (MIT), OpenSimplex2 (public
  domain / Unlicense).
- Public algorithms and data with no restrictions: Dryden turbulence (MIL-F-8785C),
  ISA atmosphere, NOAA solar position, WGS84 formulas, World Magnetic Model (public domain).
- Simplex noise patent (US 6 867 776) expired in 2022, so it's no longer a concern.
- Astronomy algorithms from published sources (Meeus, *Astronomical Algorithms*; NREL SPA;
  Espenak–Meeus ΔT) are re-implemented from the equations. NREL's SPA C code has its own
  licence terms, so it must not be copied — only the published algorithm is used.

### 12.4 Assets and data
Assets are licensed separately from code and need their own entries in
`THIRD_PARTY_NOTICES.md` (REUSE `.license` files or `REUSE.toml`).

| Asset type | Allowed sources | Notes |
|-----------|-----------------|-------|
| Online elevation data (and later online orthophotos) | **Not bundled** — fetched at runtime from web APIs into the user's local cache | Not redistributed by the project, so data licences don't affect the code licence. Obligations come from each provider's **terms of use** (§12.4.1). |
| Orthophoto files shipped with built-in missions | Public domain (e.g. USGS NAIP) or open licences allowing redistribution with attribution (e.g. national orthophoto programmes under CC BY 4.0) | Same handling as elevation files: REUSE `.license` entry, `orthophotoAttribution`, `THIRD_PARTY_NOTICES.md`. Orthophotos are large — keep shipped areas small. Never ship imagery from commercial map services. |
| COG and LAS/LAZ files shipped with built-in missions | Public domain (USGS 3DEP DEMs and lidar, SRTM, NASADEM) or open licences that allow redistribution with attribution (e.g. **Copernicus DEM GLO-30**, national lidar programmes under CC BY 4.0) | Each file gets a REUSE `.license` entry and its attribution in the mission's `attribution` field and `THIRD_PARTY_NOTICES.md`. Don't ship data under non-redistributable or non-commercial terms. Lidar is large: keep shipped tiles small, or document where to download them into the geodata library. User missions are the user's responsibility. |
| 3D models, textures, HDRIs | **CC0** preferred (e.g. Poly Haven, ambientCG); CC-BY acceptable with attribution | No CC-BY-NC / CC-BY-SA in the repository unless kept separate and clearly labelled. |
| Astronomical series | VSOP87 (Bretagnon & Francou 1988, IMCCE) — published without an explicit licence | **Decision:** the truncated, converted table (`vsop87_earth_data.cpp`) is distributed under the project's **MIT** licence, crediting the authors in the file, `THIRD_PARTY_NOTICES.md` and `debian/copyright`. |
| Star catalogue | Yale Bright Star Catalogue (BSC5, Hoffleit & Warren 1991) — freely distributed by NASA ADC/CDS without an explicit licence | **Decision:** the converted extract (`assets/sky/bsc5.csv`) is distributed under the project's **MIT** licence, crediting the authors in `THIRD_PARTY_NOTICES.md`, `debian/copyright` and the file's header comment. |
| Geomagnetic model | World Magnetic Model WMM2025 coefficients and test values (NOAA NCEI and British Geological Survey) — U.S. Government material, public domain | Converted into `src/core/geo/wmm_data.cpp` (MIT for the project's code around it) and `tests/fixtures/geo/wmm_test_values.csv`; credited, with the 17 U.S.C. 403 notice, in `THIRD_PARTY_NOTICES.md` and `debian/copyright`. |
| Moon textures | NASA (LRO / Clementine) imagery — public domain (US Government work) | Credit NASA. Avoid Milky-Way images under CC BY-SA unless kept separate (§12.4). |
| Reference data for tests | JPL Horizons / USNO outputs (US Government, public domain) | Stored as test fixtures with source notes. |
| Fonts (HUD) | SIL OFL 1.1 (e.g. Inter, DejaVu — Bitstream Vera license) | OFL allows bundling with MIT software; ship the font license. |
| Missions, drone profiles | Authored by the project → MIT | — |

#### 12.4.1 Data provider terms of use
- A provider becomes a **built-in preset** only if its terms allow access from third-party
  applications, local caching, and use in an open-source/commercial app as applicable.
  Record the terms URL and licence in the preset.
- Show the required **attribution** whenever the data is visible (FR-TER-O13).
- Respect cache restrictions (`allowCache`, `maxAgeDays`) and rate limits.
- Cached tiles are for the local user only; scene/mission export never includes them.
- API keys belong to the user; the project never ships a shared key.
- **OpenTopography specifics:** "one user, one key" (keys can't be shared, published or
  embedded); show the OpenTopography acknowledgement and the dataset's citation/licence
  (e.g. Copernicus for COP30); respect the daily call limits. Free distribution of this
  MIT project is fine; **anyone selling a product that integrates OpenTopography keys
  needs an OpenTopography Enterprise key**.

### 12.5 Distribution checklist
1. `LICENSE` (MIT) at the repo root, installed to `/opt/drone_simulator/share/doc/`, and
   summarised in `/usr/share/doc/drone-simulator/copyright` (DEP-5).
2. `THIRD_PARTY_NOTICES.md` listing every bundled library and asset with its license
   text, installed alongside binaries and viewable in *Help → About → Licenses*.
3. Qt, GDAL, libcurl, SDL3 and QtKeychain are Debian dependencies, not bundled: the
   project redistributes no LGPL/GPL-encumbered binaries.
4. If a bundled build is ever made (other distributions/Windows): Qt linked dynamically with
   licence and source offer, and GDAL built without copyleft optional dependencies (§12.2).
5. CI: `reuse lint` passes; a dependency check fails the build if a GPL-only Qt module
   or an unapproved library is linked.

---

## Appendix A. Acronyms and abbreviations

### A.1 Requirement ID prefixes

| Prefix | Area |
|--------|------|
| FR / NFR | Functional / non-functional requirement |
| FR-AST | Time and astronomy (`core/astro`) |
| FR-DB | User database and storage |
| FR-GEO | Geo-referencing (`core/geo`) |
| FR-GFX | Graphics engine |
| FR-GND | Ground drone |
| FR-IN | Input devices |
| FR-ITM | Items manager |
| FR-LOOP | Main simulation loop |
| FR-MIS | Missions |
| FR-NET | Network and data cache |
| FR-PHY | Flight physics |
| FR-PKG | Packaging |
| FR-TER | Terrain generator (`-O` online, `-F` files) |
| FR-UI | User interface |
| FR-WIND | Wind levels |
| FR-WX | Weather |
| FR-X | Cross-cutting concerns |
| NFR-MNT | Maintainability |
| NFR-PERF | Performance |
| NFR-REAL | Realism |
| NFR-REL | Reliability |
| NFR-SEC | Security and privacy |
| NFR-USE | Usability |

### A.2 Flight, vehicles and sensors

| Acronym | Meaning |
|---------|---------|
| AETR / TAER | Channel orders of an RC transmitter: Aileron-Elevator-Throttle-Rudder / Throttle-Aileron-Elevator-Rudder |
| AGL | Above ground level |
| AMSL | Above mean sea level |
| EKF | Extended Kalman filter |
| ESC | Electronic speed controller (drives a brushless motor) |
| FOV | Field of view |
| FPV | First-person view (camera on the drone) |
| GPS | Global Positioning System |
| HDG | Heading (HUD read-out) |
| HDOP | Horizontal dilution of precision (GPS quality) |
| HIL / SITL | Hardware in the loop / software in the loop |
| HUD / OSD | Head-up display / on-screen display: flight data drawn over the 3D view |
| IMU | Inertial measurement unit (gyro + accelerometer) |
| ISA | International Standard Atmosphere |
| KV | Motor velocity constant (rpm per volt) |
| LiPo / Li-ion | Lithium-polymer / lithium-ion battery |
| LOS / VLOS | Line of sight / visual line of sight (pilot to drone) |
| MN | Magnetic north (compass marker) |
| OCV | Open-circuit voltage (battery) |
| PID | Proportional-integral-derivative controller |
| QNH | Altimeter setting: sea-level pressure that makes a barometer read the field elevation |
| RC | Radio control (transmitter and receiver) |
| RK4 | Classical fourth-order Runge–Kutta integrator |
| RPM | Revolutions per minute |
| RTH | Return to home |
| SoC | State of charge (battery) |
| UAV | Unmanned aerial vehicle |
| UGV | Unmanned ground vehicle (the ground drone) |

### A.3 Geodesy, terrain and imagery

| Acronym | Meaning |
|---------|---------|
| COG / COPC | Cloud-Optimized GeoTIFF / Cloud-Optimized Point Cloud |
| COP30 / COP90 | Copernicus DEM, 30 m / 90 m (GLO-30 / GLO-90) |
| CRS | Coordinate reference system |
| DD / DMS | Decimal degrees / degrees-minutes-seconds |
| DECL | Magnetic declination (HUD read-out) |
| DEM / DTM / DSM | Digital elevation / terrain / surface model |
| ECEF | Earth-centred, Earth-fixed coordinates |
| EGM96 / EGM2008 | Earth Gravitational Model geoids (orthometric heights) |
| ENU | Local East-North-Up frame |
| EPSG | EPSG Geodetic Parameter Dataset codes (e.g. EPSG:4326 = WGS84 lat/lon) |
| IDW | Inverse distance weighting (interpolation) |
| LAS / LAZ | ASPRS lidar point-cloud format / its compressed form |
| LOD | Level of detail |
| NAIP | National Agriculture Imagery Program (USGS/USDA orthophotos) |
| OGC | Open Geospatial Consortium |
| SMRF | Simple Morphological Filter (lidar ground classification) |
| SRTM | Shuttle Radar Topography Mission (DEM; GL1/GL3 = 1″/3″ grids) |
| TIN | Triangulated irregular network |
| UPS | Universal Polar Stereographic |
| UTM | Universal Transverse Mercator |
| VLR | Variable-length record (LAS header) |
| VRT | GDAL virtual raster |
| WCS / WMS / WMTS | OGC Web Coverage / Map / Map Tile Service |
| WGS84 | World Geodetic System 1984 |
| WKT | Well-known text (CRS description) |
| WMM | World Magnetic Model (WMM2025: valid 2025–2030) |
| XYZ / TMS | Web map tile URL schemes (`{z}/{x}/{y}`) / Tile Map Service |

### A.4 Astronomy and sky

| Acronym | Meaning |
|---------|---------|
| BSC5 | Yale Bright Star Catalogue, 5th revised edition |
| ELP | Éphéméride Lunaire Parisienne (lunar theory, ELP-2000/82) |
| EV | Exposure value |
| HDR | High dynamic range |
| SPA | Solar Position Algorithm (NREL) |
| TT | Terrestrial Time |
| UTC | Coordinated Universal Time |
| VSOP87 | Variations Séculaires des Orbites Planétaires (planetary theory, 1987) |

### A.5 Software, graphics and build

| Acronym | Meaning |
|---------|---------|
| AABB / OBB | Axis-aligned / oriented bounding box |
| API | Application programming interface |
| APT | Debian package manager (Advanced Package Tool) |
| BVH | Bounding volume hierarchy |
| CI | Continuous integration |
| CRUD | Create, read, update, delete |
| DB | Database |
| DEP-5 | Debian machine-readable copyright format |
| DSA | Direct State Access (OpenGL 4.5) |
| GDAL / PDAL | Geospatial / Point Data Abstraction Library |
| GL / GLSL | OpenGL / OpenGL Shading Language |
| GPU / CPU | Graphics / central processing unit |
| GUI / UI | Graphical user interface / user interface |
| GUID | Globally unique identifier (joystick identity) |
| HID | Human interface device (USB) |
| HTTP(S) / TLS | Hypertext Transfer Protocol (Secure) / Transport Layer Security |
| JSON | JavaScript Object Notation |
| LRU | Least recently used (cache eviction) |
| MSAA | Multisample anti-aliasing |
| RAII | Resource acquisition is initialisation (C++ ownership idiom) |
| RNG | Random number generator |
| SDL | Simple DirectMedia Layer (SDL3: joysticks and gamepads) |
| SQL | Structured Query Language |
| URL | Uniform resource locator |
| VAO | Vertex array object (OpenGL) |
| WAL | Write-ahead logging (SQLite journal mode) |

### A.6 Organisations, data sources and licences

| Acronym | Meaning |
|---------|---------|
| ADC / CDS | NASA Astronomical Data Center / Centre de Données astronomiques de Strasbourg |
| ASPRS | American Society for Photogrammetry and Remote Sensing |
| BGS | British Geological Survey |
| BSD / MIT | Permissive software licences |
| CC0 / CC BY / CC BY-SA / CC BY-NC | Creative Commons licences (public-domain dedication / attribution / share-alike / non-commercial) |
| GPL / LGPL | GNU (Lesser) General Public License |
| IMCCE | Institut de Mécanique Céleste et de Calcul des Éphémérides |
| JPL | NASA Jet Propulsion Laboratory (Horizons ephemerides) |
| LRO / LROC | Lunar Reconnaissance Orbiter / its camera |
| NASA | National Aeronautics and Space Administration |
| NCEI / NOAA | National Centers for Environmental Information / National Oceanic and Atmospheric Administration |
| NREL | National Renewable Energy Laboratory |
| OFL | SIL Open Font License |
| REUSE | FSFE specification for licence information in every file |
| SPDX | Software Package Data Exchange (licence identifiers) |
| USGS / 3DEP | U.S. Geological Survey / its 3D Elevation Program |
| USNO | U.S. Naval Observatory |
