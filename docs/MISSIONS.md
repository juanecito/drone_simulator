# Missions

A mission is **one JSON file** — the single source of truth for everything needed to play it:
terrain, time and weather, vehicle, rules, items, objectives, scheduled failures and scoring
(SPECIFICATION §4.9). This page explains how missions behave and how to write them; the
[field reference](#field-reference) at the end is generated from the schema.

## Files and folders

- **Format:** UTF-8, strict JSON (no comments), extension `.json`, at most 5 MB. The
  extension `.mission.json` is recommended: such files are always treated as missions. Any
  other `.json` file counts as a mission when its top-level object has `schemaVersion` or
  `objectives`, or when it is not valid JSON (it is then listed as invalid, with the parse
  error); other JSON files, `*.schema.json` and files whose name starts with `.` are ignored.
- **Schema:** `assets/missions/mission.schema.json` (JSON Schema draft 2020-12). The application
  validates with this same file; add `"$schema": "<path to mission.schema.json>"` to get
  completion and validation in editors such as VS Code.
- **Unknown properties are errors**, except extension fields whose name starts with `x-`
  (`"x-author": "…"`), which are allowed anywhere and ignored.
- **`schemaVersion`** is required (currently `1`). A file written for a newer version is
  rejected with *"version N is newer than this application supports"*.
- **Locations:** built-in missions in `/opt/drone_simulator/share/missions/`, the user's in
  `~/.drone_simulation/missions/`. **Every valid mission in these folders is added to the list
  of missions automatically at start-up** — just drop the file in the folder. A mission is
  either a single mission file, or a folder with exactly one mission file at its top plus its
  data (`dem/`, `ortho/`). Invalid files are listed too, marked invalid with their errors. Each
  mission `id` must be unique; a user mission cannot reuse the id of a built-in one.
- **Default mission:** at every start the application makes sure your mission folder holds
  *First flight* ([`assets/default-missions/first-flight.mission.json`](../assets/default-missions/first-flight.mission.json)):
  take off, climb to 10 m, fly to a beacon 60 m north and land back on the pad. It is copied
  only when no file of that name and no mission with the id `first-flight` exists, so your
  edits are kept; if you delete it, it comes back at the next start. It is a good starting
  point for your own missions — copy it under a new file name and change its `id`.
- **Import mission** copies a file, or a whole mission folder (symbolic links are not copied),
  into `~/.drone_simulation/missions/`.

## The Missions tab

The **Missions** tab lists every mission found at start-up, grouped by campaign chapter:
missions without a chapter under *Free missions*, files with errors under *Invalid missions*.
Each row shows the vehicle type, your best stars and fastest time, and whether the mission is
*Available*, *Locked* (hover over it to see what unlocks it), *Unavailable* (data missing,
e.g. an elevation file) or *Invalid*. Selecting a mission shows its briefing, objectives,
conditions, rules, star thresholds and your results — or, for an invalid file, every error
with its JSON path. After adding or editing files press **Reload** (or *Missions › Reload
Missions*); **Open Folder** opens your mission folder.

## Validating a mission

```sh
drone_simulator --validate-mission my-mission.mission.json [--validate-mission other.mission.json]
```

prints one line per problem and exits with `0` when every file is valid, `1` otherwise. No
display is needed, so it can run in CI. Messages name the file, the JSON path and the problem:

```
my-mission.mission.json: error: objectives[1].target: unknown item "pad2"
my-mission.mission.json: error: objectives[0].altitudeAglM: 150 m cannot be reached: the height limit is 120 m AGL
my-mission.mission.json: warning: scoring.stars[1].maxTimeS: the 3rd star is easier than the 2nd
```

Validation runs in two passes:

1. **Schema:** structure, types, ranges, required and unknown fields.
2. **Semantic checks** (only when the schema passes):
   - ids are unique; every `target`, `gates[]`, `pickup`, `drop` and `after` refers to an
     existing item or objective **of the right type** (a `land` target must be a
     `landingPad`, …); `after` has no cycles;
   - objectives and rules suit the **vehicle type** (see the table below);
   - the vehicle profile exists for that type (or `customProfile.vehicleType` matches);
   - item properties apply to the item type (`widthM` only for gates, `polygon` only for
     areas and zones, …);
   - positions lie **inside `terrain.extent`**; geographic positions need a terrain origin;
     no-fly zones do not contain the spawn point; geofences do;
   - thresholds are **reachable**: altitudes of objectives, waypoints, gates and inspection
     targets respect the height limit;
   - **weather is plausible** (FR-WX-05): no snow above 4 °C, fog or mist need ≥ 80 %
     humidity, cloud tops above their bases, no cloud base below the fog top, timeline entries
     in increasing time (a thunderstorm with calm wind is a warning);
   - `utc` is a valid time in the supported years and `timeZone` a known IANA zone;
   - file terrain: the terrain object is valid (DSM is not supported in v1.0) and file paths
     follow the path rules (no absolute paths, no `..`, no symlinks leaving the folder).

**Warnings** never make a mission invalid (e.g. a star threshold that can never matter).
A valid mission can still be **unavailable** — listed with the reason but not playable — for
example when an elevation file is missing or does not match its `sha256`, or an online
provider has no API key or the area is not downloaded yet (FR-MIS-02).

## Positions and coordinates

Every position — `spawn`, items, objective `position` — uses **exactly one** of:

| Form | Example | Meaning |
|------|---------|---------|
| `geo` | `"geo": [46.5123, 7.9811, 2.0]` | WGS84 latitude, longitude (degrees) and altitude (m) |
| `utm` | `"utm": {"zone": "32T", "easting": 421000, "northing": 5151000, "alt": 2}` | UTM zone with latitude band, metres |
| `local` | `"local": [380, 0, -520]` | metres from the terrain origin in the **engine frame**: x east, y up, **z south** (so `-520` is 520 m north) |

`altRef` (`"AGL"` by default, or `"AMSL"`) says what the altitude is measured from. AGL items
follow the terrain: they are placed `alt` metres above the ground under them, and move with it
when the terrain changes. `geo` and `utm` positions need a geographic origin
(`terrain.origin`, or the origin of the elevation files for file terrain).

Item **polygons** (`surveyArea`, `noFlyZone`, `geofence`) are lists of `[x, z]` offsets in
metres from the item position, in the same engine frame, rotated by the item's `headingDeg`.
Headings are degrees clockwise from true north.

## Items

| Type | Shape and defaults | Notes |
|------|--------------------|-------|
| `spawn` | point | The mission's start point is `spawn`; this item type is for scenes. |
| `pilot` | point | Pilot position for the line-of-sight rule and camera; defaults to the spawn point. At most one. |
| `waypoint` | sphere, `radiusM` 2 | |
| `gate` | opening `widthM` × `heightM` (2 × 2 m) centred on the position | Direction-aware: pass it in the direction of `headingDeg`. |
| `landingPad` | disc, `radiusM` 1 | |
| `pickupZone`, `dropZone` | cylinder `radiusM` 3 from 1 m below to `heightM` (10 m) above, or a box with `sizeM` [w, l] | |
| `parkingZone` | box `sizeM` [w, l] (2 × 3 m), `heightM` 2, rotated by `headingDeg` | Target heading for `park`. |
| `inspectionTarget` | sphere, `radiusM` 0.5 | |
| `surveyArea` | prism from `polygon`, or a cylinder | |
| `noFlyZone` | keep-out prism (`polygon`) or cylinder (`radiusM`), `bottomM`/`topM` optional | Entering it fails the mission. |
| `geofence` | keep-in prism or cylinder | Leaving every geofence fails the mission. |
| `obstacle` | `shape`: `box` (`sizeM` [w, h, l]), `cylinder`, `tree`, `building`, `powerLine`, `windTurbine` | Solid: blocks flight and the pilot's line of sight. |
| `ramp`, `bridge` | box `sizeM` | Ground drones (M6). |
| `payload` | small box | |

## Objectives

| Objective | Aerial | Ground | Completed when | Defaults |
|-----------|:------:|:------:|----------------|----------|
| `reachAltitude` | ✓ | — | height above the terrain ≥ `altitudeAglM` − `toleranceM`, held `holdS` | tolerance 0.5 m, hold 0 s |
| `reachWaypoint` | ✓ | ✓ (2D) | inside the waypoint (`target`) or `radiusM` around `position`, within the altitude band (aerial) and below `maxSpeedMs`, held `holdS` | radius from the item or 2 m |
| `hover` | ✓ | — | within `toleranceM` horizontally of the target or position and `altitudeToleranceM` of `altitudeAglM`, airborne, continuously for `durationS` | tolerances 1 m |
| `land` | ✓ | — | touched down on the pad after flying, settled (< 0.3 m/s for 1 s) within `maxErrorM` of its centre and `maxHeadingErrorDeg` of its heading. A touchdown faster than `maxTouchdownSpeedMs` **fails** the objective (hard landing). | error 0.5 m, touchdown 1 m/s |
| `gateCourse` | ✓ | ✓ | all `gates` passed in order, in their direction, `laps` times. A wrong gate costs points, or fails the objective with `strictOrder`. | 1 lap |
| `deliver` | ✓ | ✓ | cargo picked up in `pickup` (stay ≤ 1 m above the ground and slower than 0.5 m/s for 1 s; ground: stopped) and released the same way in `drop` (a `dropZone`, `landingPad` or `parkingZone`) slower than `maxDropSpeedMs`. The cargo mass `cargoKg` is carried in between. | |
| `inspect` | ✓ | ✓ | the target kept within `maxDistanceM` and `maxAngleDeg` of the FPV camera axis, in sight, for `durationS` in total | 30 m, 20°, 5 s |
| `survey` | ✓ | ✓ | `coveragePct` of the area covered by the `swathWidthM` footprint, within the altitude band (aerial) | 90 %, swath 20 m (ground 2 m) |
| `returnHome` | ✓ | ✓ | back within `radiusM` of the spawn point after leaving it, on the ground and stopped for 1 s | 2 m |
| `park` | — | ✓ | inside the parking zone within `maxErrorM` of its centre and `maxHeadingErrorDeg` of its heading, slower than `maxSpeedMs`, held `holdS` | 0.5 m, 10°, 0.1 m/s, 1 s |

**Order.** By default objectives run in parallel; `after: ["id", …]` makes one wait for
others. With `"objectiveOrder": "sequential"` each objective also waits for every required
objective listed before it. **Optional** objectives (`"optional": true`) are not needed for
success and add their `bonus` (default 100) to the score. When a required objective fails the
mission fails; objectives waiting for a failed objective fail too.

## Rules

| Rule | Default | Notes |
|------|---------|-------|
| `maxAltitudeAglM` | 120 | Aerial only. Height above the terrain directly below. Warning from 100 m (5/6 of the limit). `null` removes the limit. |
| `vlos.enabled` | true | Visual line of sight between the pilot's eye (1.7 m above the pilot position) and the vehicle. |
| `vlos.maxDistanceM` | 500 | Horizontal distance, further limited by the weather's visibility. Warning at 90 %. |
| `vlos.maxHiddenS` | 5 | Longest time hidden behind terrain or solid items. |
| `graceS` | 3 | How long a rule may be broken before the mission fails. |
| `timeLimitS` | none | |
| `minBatteryPct` | 0 | Below this the mission fails; an empty battery always fails it. |
| `requireLightsOn` | false | At night VLOS requires the lights anyway. |
| `daylightOnly` | false | Fails after civil dusk (sun below −6°). |
| `endAt` | none | `"sunset"` / `"sunrise"`: the mission ends then (failed unless already complete). |
| `maxStuckS` | 30 | Ground only. |
| `maxCollisionSpeedMs` | 2 | Ground only: a harder collision fails the mission. |
| `allowKeyboard` | false | Keyboard flying allowed. |
| `showMarkers` | true | Draw item markers and guidance. |

Rules apply while the vehicle is armed. Overrides of the default flight rules are listed in the
briefing as **Special rules** (FR-MIS-06c). Ground missions apply VLOS only.

**Other failures:** crash, water, leaving the terrain extent, entering a no-fly zone, leaving
the geofence, an empty battery; ground drones also fail on rollover, being stuck and hard
collisions.

**Failure injection** (`failures[]`, FR-MIS-07): at `atS` seconds of mission time the given
failure (`gpsDrift`, `gpsLoss`, `motorFailure`, `batteryDrop`, `compassError`, `rcLoss`,
`baroDrift`) is handed to the simulation with its parameters. The physical effects of the
injected failures arrive in M7.

## Lifecycle

`Briefing → PreFlight → InProgress → Succeeded / Failed / Aborted → Debrief`. In PreFlight the
vehicle must be on the ground at the spawn point with a full battery; the mission starts (and
its clock with it) when the vehicle is armed — or, for a ground drone, starts moving. *Retry*
starts a clean attempt from PreFlight.

## Flying a mission

1. Select a mission in the **Missions** tab and press **Start** (or double-click it, or
   *Missions › Start Mission…*). The **briefing** shows a map of the area (items, take-off
   point, where you stand), the objectives, rules, weather and the day/night situation. If
   the mission allows it, choose the vehicle profile there. Press **Fly**.
2. The panel hides and the drone waits on the take-off point. **Arm** it (Space, or the arm
   switch of your transmitter): the clock starts. The HUD shows the mission panel (state,
   timer, objectives with the distance to each target) and markers on the current targets.
3. Rule warnings (height limit, distance, line of sight, no-fly zones, lights, …) appear in
   red; a rule still broken after the grace time (`rules.graceS`, 3 s by default) fails the
   mission. Trees, masts, buildings and gate frames are **solid**: touching one with spinning
   propellers, or hitting it hard, is a crash.
4. When the attempt ends — success, failure, or *Missions › Abort Mission* — it is saved at
   once with your progress. The **debrief** follows: stars, score breakdown, the path you
   flew, and your personal best. Choose **Retry**, **Next mission** or **Back to free flight**
   (your free-flight scene, vehicle and camera come back as they were).

*Missions › Import Mission…* (a file) and *Import Mission Folder…* (a folder with one mission
file and its data) copy a mission into your folder and select it.

## Built-in missions

| Chapter | Mission | What it trains |
|---------|---------|----------------|
| Basics | Hold steady | hover over a pad for 10 s, land |
| Basics | Square circuit | waypoints in order, return home, land |
| Basics | Precision landing | landing within 0.5 m on a hillside pad, gently |
| Basics | Crosswind transit | waypoints in a moderate crosswind, battery reserve |
| Racing | Gate run | five gates, one lap, racing quad |
| Racing | Forest slalom | eight gates between solid trees, strict order |
| Operations | Parcel delivery | pick up 0.5 kg, deliver to a farm, return, land |
| Operations | Mast inspection | keep an antenna in view for 8 s next to a solid mast |
| Operations | Field survey | 80 % coverage of a field between 25 and 45 m |
| Operations | Restricted airspace | fly around a no-fly zone |
| Operations | Dusk patrol | after sunset, lights on, visual line of sight |
| Operations | Valley relay | alpine terrain, ridges hiding the drone |
| Real terrain | Montserrat ridge | online terrain (needs the online elevation source, M4) |

The first mission of each chapter is open; each further one needs a star on the one before.

## Scoring and stars

| Part | Points |
|------|--------|
| Completion | 500 × (completed required objectives / required objectives), + 500 on success |
| Time (success) | 300 × (1 − time / reference), reference = `timeLimitS`, else 2 × the 2nd-star `maxTimeS`, else 600 s |
| Precision | 200 × mean of (1 − error / max error) over completed objectives that measure a position error (land, park, hover, reachWaypoint, deliver, returnHome); 200 when there are none |
| Battery (success) | the charge left, in % |
| Smoothness (success) | 100 × e^(−mean jerk / 30 m/s³) |
| Bonus | `bonus` of each completed optional objective |
| Penalties | −25 per collision, −10 per rule warning, −25 per wrong gate |

The total is never negative. **Stars:** success gives the first star; `scoring.stars[0]`
the second and `scoring.stars[1]` the third, each only if every condition in it holds
(`maxTimeS`, `maxErrorM` — the worst position error —, `minBatteryPct`, `minScore`) and the
previous star was earned. A failed or aborted attempt has no stars.

## Campaign

Missions are grouped by `chapter` and ordered by `order`. The first mission of a chapter is
unlocked; every other one needs at least one star on the mission before it. An explicit
`"unlock": {"requires": ["mission-id", …], "minStars": 2}` replaces that rule. Missions without
a chapter are always available. Progress (attempts, best stars, best score, best time) is
stored per user in the database; it survives when a mission file disappears.

## Examples

The two examples of SPECIFICATION §4.9.1 are kept as test fixtures and pass validation, the
default mission is the simplest complete mission, and the built-in missions in
[`assets/missions/`](../assets/missions/) show every aerial objective type:

- [`assets/default-missions/first-flight.mission.json`](../assets/default-missions/first-flight.mission.json)
  — aerial, procedural terrain, take-off, waypoint and landing in sequence.

- [`tests/fixtures/missions/precision-landing-01.mission.json`](../tests/fixtures/missions/precision-landing-01.mission.json)
  — aerial, online terrain, precision landing with a scheduled GPS drift.
- [`tests/fixtures/missions/rover-delivery-01.mission.json`](../tests/fixtures/missions/rover-delivery-01.mission.json)
  — ground, procedural terrain, delivery and parking.

## Field reference

<!-- BEGIN GENERATED: mission-reference (scripts/docs/gen_mission_reference.py) -->

*Generated from `assets/missions/mission.schema.json` — do not edit by hand.*

#### Mission (top level)

One mission of the Drone Simulator (SPECIFICATION §4.9.1, docs/MISSIONS.md). Unknown properties are errors, except extension fields whose name starts with "x-".

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `schemaVersion` | 1 | yes |  |  |
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `title` | string | yes | min length 1, max length 120 |  |
| `briefing` | string |  | max length 8000 |  |
| `difficulty` | integer |  | ≥ 1, ≤ 5 |  |
| `chapter` | id |  | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `order` | integer |  | ≥ 0, ≤ 10000 |  |
| `unlock` | object |  |  |  |
| `terrain` | object (terrain) | yes |  |  |
| `environment` | object (environment) | yes |  |  |
| `vehicle` | object (vehicle) | yes |  |  |
| `spawn` | object (spawn) | yes |  |  |
| `rules` | object (rules) |  |  |  |
| `items` | array of object (item) |  | max items 10000 |  |
| `objectiveOrder` | "parallel" \| "sequential" |  |  |  |
| `objectives` | array of object (objective) | yes | min items 1, max items 200 |  |
| `failures` | array of object (failure) |  | max items 100 |  |
| `scoring` | object |  |  |  |

#### terrain — procedural

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `origin` | object (origin) |  |  | Geographic origin of the terrain. |
| `extent` | object (extent) |  |  | Bounded terrain centred on the origin (FR-TER-04a). |
| `seed` | integer | yes | ≥ 0 | Noise seed. |
| `preset` | string |  |  | Built-in preset to start from, e.g. "rolling-hills". |
| `params` | object (procParams) |  |  | Overrides of the procedural parameters (SPECIFICATION §4.5.2, §4.10 scene.terrain params). |

#### terrain — online

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `origin` | object (origin) | yes |  | Geographic origin of the terrain. |
| `extent` | object (extent) |  |  | Bounded terrain centred on the origin (FR-TER-04a). |
| `elevationProvider` | string | yes | min length 1 |  |
| `dataset` | string |  | min length 1 | Provider dataset, e.g. "COP30". |
| `imageryProvider` | string \| null |  |  |  |
| `maxResolutionM` | number |  | > 0 |  |

#### terrain — file

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `origin` | object (origin) |  |  | Geographic origin of the terrain. |
| `extent` | object (extent) |  |  | Bounded terrain centred on the origin (FR-TER-04a). |
| `format` | "raster" \| "las" \| "laz" | yes |  |  |
| `elevationFiles` | array of object (fileRef) | yes | min items 1, max items 1024 |  |
| `crs` | string |  |  |  |
| `verticalDatum` | string |  |  |  |
| `attribution` | string |  |  |  |
| `band` | integer |  | ≥ 1, ≤ 1000 |  |
| `surface` | "dtm" \| "dsm" |  |  |  |
| `resolutionM` | number |  | ≥ 0.25, ≤ 100 |  |
| `interpolation` | "tin" \| "idw" \| "max" |  |  |  |
| `classifyGround` | boolean |  |  |  |
| `orthophotoFiles` | array of object (fileRef) |  | max items 4096 |  |
| `orthophotoCrs` | string |  |  |  |
| `orthophotoAttribution` | string |  |  |  |
| `orthophotoBands` | array of integer |  |  |  |

#### terrain.params (procedural overrides)

Overrides of the procedural parameters (SPECIFICATION §4.5.2, §4.10 scene.terrain params).

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `octaves` | integer |  | ≥ 1, ≤ 16 |  |
| `featureSizeM` | number |  | > 0 |  |
| `lacunarity` | number |  | ≥ 1.5, ≤ 4 |  |
| `persistence` | number |  | ≥ 0.05, ≤ 0.95 |  |
| `amplitudeM` | number |  | ≥ 0, ≤ 8000 |  |
| `baseHeightM` | number |  | ≥ -9000, ≤ 9000 |  |
| `mountains` | number |  | ≥ 0, ≤ 1 |  |
| `warp` | number |  | ≥ 0, ≤ 1 |  |
| `erosion` | number |  | ≥ 0, ≤ 1 |  |
| `edge` | "cliff" \| "fade" \| "water" |  |  |  |
| `waterLevelM` | number |  | ≥ -9000, ≤ 9000 |  |
| `snowLineM` | number |  | ≥ -9000, ≤ 9000 |  |
| `flatZones` | array of object |  | max items 64 |  |

#### origin

Geographic origin of the terrain.

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `geo` | array[3] | yes | min items 3 | [latitude °, longitude °, altitude m] (WGS84). |
| `altRef` | "AGL" \| "AMSL" |  |  | Altitude reference: above the terrain (default) or above mean sea level. |

#### extent

Bounded terrain centred on the origin (FR-TER-04a).

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `widthM` | number | yes | ≥ 256, ≤ 200000 |  |
| `lengthM` | number | yes | ≥ 256, ≤ 200000 |  |

#### File reference (elevationFiles[], orthophotoFiles[])

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `path` | string | yes | min length 1, max length 1024 |  |
| `sha256` | string |  | pattern `^[0-9a-fA-F]{64}$` |  |

#### environment

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `utc` | string | yes | pattern `^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}(:[0-9]{2}(\.[0-9]+)?)?Z$` | Mission start, ISO 8601 UTC. |
| `timeZone` | string |  | min length 1 | IANA time zone used for display, e.g. "Europe/Madrid". |
| `lightPollution` | integer |  | ≥ 1, ≤ 9 |  |
| `weather` | object (weather) |  |  | Seeded weather generator settings (§4.2.3). |

#### environment.weather

Seeded weather generator settings (§4.2.3).

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `preset` | "clear" \| "few-clouds" \| "scattered-clouds" \| "broken-clouds" \| "overcast" \| "mist" \| "fog" \| "drizzle" \| "light-rain" \| "heavy-rain" \| "light-snow" \| "heavy-snow" \| "thunderstorm" | yes |  |  |
| `temperatureC` | number |  | ≥ -60, ≤ 60 |  |
| `humidityPct` | number |  | ≥ 0, ≤ 100 |  |
| `dewPointC` | number |  | ≥ -80, ≤ 60 |  |
| `pressureHpa` | number |  | ≥ 850, ≤ 1100 |  |
| `wind` | object (wind) |  |  |  |
| `clouds` | array of object (cloudLayer) |  | max items 3 |  |
| `precipitation` | object (precipitation) |  |  |  |
| `visibilityM` | number |  | ≥ 10, ≤ 100000 |  |
| `fog` | object |  |  |  |
| `seed` | integer | yes | ≥ 0 |  |
| `timeline` | array of object (weatherChange) |  | max items 64 |  |
| `forecast` | boolean |  |  |  |

#### weather.timeline[]

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `preset` | "clear" \| "few-clouds" \| "scattered-clouds" \| "broken-clouds" \| "overcast" \| "mist" \| "fog" \| "drizzle" \| "light-rain" \| "heavy-rain" \| "light-snow" \| "heavy-snow" \| "thunderstorm" |  |  |  |
| `temperatureC` | number |  | ≥ -60, ≤ 60 |  |
| `humidityPct` | number |  | ≥ 0, ≤ 100 |  |
| `dewPointC` | number |  | ≥ -80, ≤ 60 |  |
| `pressureHpa` | number |  | ≥ 850, ≤ 1100 |  |
| `wind` | object (wind) |  |  |  |
| `clouds` | array of object (cloudLayer) |  | max items 3 |  |
| `precipitation` | object (precipitation) |  |  |  |
| `visibilityM` | number |  | ≥ 10, ≤ 100000 |  |
| `fog` | object |  |  |  |
| `atS` | number | yes | ≥ 0 |  |
| `transitionS` | number |  | ≥ 0 |  |

#### wind

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `level` | integer |  | ≥ 0, ≤ 10 | Wind level 0–10 (Beaufort-based, §4.2.1). |
| `fromDeg` | number |  | ≥ 0, ≤ 360 | Direction the wind blows from. |

#### Cloud layer (clouds[])

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `type` | "cumulus" \| "stratocumulus" \| "stratus" \| "cirrus" \| "cumulonimbus" | yes |  |  |
| `coverOktas` | integer | yes | ≥ 0, ≤ 8 |  |
| `baseM` | number | yes | ≥ 0, ≤ 20000 |  |
| `topM` | number |  | ≥ 0, ≤ 20000 |  |

#### precipitation

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `type` | "none" \| "drizzle" \| "rain" \| "snow" \| "sleet" | yes |  |  |
| `intensity` | "light" \| "moderate" \| "heavy" |  |  |  |
| `rateMmH` | number |  | ≥ 0, ≤ 200 |  |

#### vehicle

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `type` | "aerial" \| "ground" | yes |  |  |
| `profile` | string |  | min length 1 | Key of a built-in vehicle profile. |
| `customProfile` | object |  |  | Full embedded vehicle profile (same format as assets/vehicles). |
| `allowProfileChange` | boolean |  |  |  |

#### spawn

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `geo` | array[3] |  | min items 3 | [latitude °, longitude °, altitude m] (WGS84). |
| `utm` | object (utm) |  |  |  |
| `local` | array[3] |  | min items 3 | [x east, y up, z south] metres from the terrain origin (engine frame); y is measured from altRef. |
| `altRef` | "AGL" \| "AMSL" |  |  | Altitude reference: above the terrain (default) or above mean sea level. |
| `headingDeg` | number |  | ≥ 0, ≤ 360 | Heading in degrees, clockwise from true north. |

#### rules

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `allowKeyboard` | boolean |  |  |  |
| `showMarkers` | boolean |  |  |  |
| `maxAltitudeAglM` | number \| null |  | > 0 | Height limit above the terrain; null removes it (default 120). |
| `vlos` | object |  |  |  |
| `graceS` | number |  | ≥ 0, ≤ 60 |  |
| `timeLimitS` | number |  | > 0 |  |
| `minBatteryPct` | number |  | ≥ 0, ≤ 100 |  |
| `maxStuckS` | number |  | > 0 |  |
| `maxCollisionSpeedMs` | number |  | > 0 |  |
| `requireLightsOn` | boolean |  |  |  |
| `daylightOnly` | boolean |  |  |  |
| `endAt` | "sunset" \| "sunrise" |  |  |  |

#### items[]

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `geo` | array[3] |  | min items 3 | [latitude °, longitude °, altitude m] (WGS84). |
| `utm` | object (utm) |  |  |  |
| `local` | array[3] |  | min items 3 | [x east, y up, z south] metres from the terrain origin (engine frame); y is measured from altRef. |
| `altRef` | "AGL" \| "AMSL" |  |  | Altitude reference: above the terrain (default) or above mean sea level. |
| `type` | "spawn" \| "obstacle" \| "gate" \| "waypoint" \| "landingPad" \| "payload" \| "pickupZone" \| "dropZone" \| "inspectionTarget" \| "surveyArea" \| "noFlyZone" \| "geofence" \| "pilot" \| "parkingZone" \| "ramp" \| "bridge" | yes |  |  |
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `name` | string |  | min length 1, max length 200 |  |
| `headingDeg` | number |  | ≥ 0, ≤ 360 | Heading in degrees, clockwise from true north. |
| `shape` | "box" \| "cylinder" \| "tree" \| "building" \| "powerLine" \| "windTurbine" |  |  |  |
| `radiusM` | number |  | > 0 |  |
| `widthM` | number |  | > 0 |  |
| `heightM` | number |  | > 0 |  |
| `sizeM` | array of number |  | min items 2, max items 3 | [width, length] or [width, height, length] in metres. |
| `polygon` | array of array[2] |  | min items 3, max items 256 | Outline as [x, z] offsets in metres from the item position (engine frame: x east, z south). |
| `bottomM` | number |  |  |  |
| `topM` | number |  |  |  |
| `massKg` | number |  | > 0 |  |
| `order` | integer |  | ≥ 0 |  |

#### utm

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `zone` | string | yes | pattern `^([1-9]|[1-5][0-9]|60)[C-HJ-NP-X]$` | Zone number and latitude band, e.g. "32T". |
| `easting` | number | yes | ≥ 100000, ≤ 900000 |  |
| `northing` | number | yes | ≥ 0, ≤ 10000000 |  |
| `alt` | number | yes | ≥ -500, ≤ 9000 |  |

#### Objective: reachAltitude

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `altitudeAglM` | number | yes | > 0 |  |
| `toleranceM` | number |  | > 0 |  |
| `holdS` | number |  | ≥ 0 |  |

#### Objective: reachWaypoint

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `minAltitudeAglM` | number |  | ≥ 0 |  |
| `maxAltitudeAglM` | number |  | > 0 |  |
| `target` | id |  | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `position` | object (objPosition) |  |  | A position without an item: exactly one of geo, utm, local. |
| `radiusM` | number |  | > 0 |  |
| `holdS` | number |  | ≥ 0 |  |
| `maxSpeedMs` | number |  | > 0 |  |

#### Objective: hover

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `target` | id |  | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `position` | object (objPosition) |  |  | A position without an item: exactly one of geo, utm, local. |
| `altitudeAglM` | number |  | > 0 |  |
| `toleranceM` | number |  | > 0 |  |
| `altitudeToleranceM` | number |  | > 0 |  |
| `durationS` | number | yes | > 0 |  |

#### Objective: land

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `target` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `maxErrorM` | number |  | > 0 |  |
| `maxTouchdownSpeedMs` | number |  | > 0 |  |
| `maxHeadingErrorDeg` | number |  | > 0, ≤ 180 |  |

#### Objective: gateCourse

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `gates` | array of id | yes | min items 1, max items 1000 | Gate ids in flying order; a gate may appear more than once. |
| `laps` | integer |  | ≥ 1, ≤ 100 |  |
| `strictOrder` | boolean |  |  |  |

#### Objective: deliver

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `pickup` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `drop` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `cargoKg` | number | yes | > 0 |  |
| `maxDropSpeedMs` | number |  | > 0 |  |

#### Objective: inspect

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `target` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `maxDistanceM` | number |  | > 0 |  |
| `maxAngleDeg` | number |  | > 0, ≤ 90 |  |
| `durationS` | number |  | > 0 |  |

#### Objective: survey

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `minAltitudeAglM` | number |  | ≥ 0 |  |
| `maxAltitudeAglM` | number |  | > 0 |  |
| `target` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `coveragePct` | number |  | > 0, ≤ 100 |  |
| `swathWidthM` | number |  | > 0 |  |

#### Objective: returnHome

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `radiusM` | number |  | > 0 |  |

#### Objective: park

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `id` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `type` | "reachAltitude" \| "reachWaypoint" \| "hover" \| "land" \| "gateCourse" \| "deliver" \| "inspect" \| "survey" \| "returnHome" \| "park" | yes |  |  |
| `title` | string |  | min length 1, max length 200 |  |
| `after` | array of id |  |  |  |
| `optional` | boolean |  |  |  |
| `bonus` | number |  | ≥ 0, ≤ 10000 |  |
| `target` | id | yes | pattern `^[a-z0-9][a-z0-9._-]{0,63}$` | Identifier: lower-case letters, digits, '.', '_' and '-', at most 64 characters. |
| `maxErrorM` | number |  | > 0 |  |
| `maxHeadingErrorDeg` | number |  | > 0, ≤ 180 |  |
| `maxSpeedMs` | number |  | > 0 |  |
| `holdS` | number |  | ≥ 0 |  |

#### failures[]

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `atS` | number | yes | ≥ 0 |  |
| `type` | "gpsDrift" \| "gpsLoss" \| "motorFailure" \| "batteryDrop" \| "compassError" \| "rcLoss" \| "baroDrift" | yes |  |  |
| `magnitudeM` | number |  | > 0 |  |
| `durationS` | number |  | > 0 |  |
| `motor` | integer |  | ≥ 1, ≤ 8 |  |
| `efficiency` | number |  | ≥ 0, ≤ 1 |  |
| `fraction` | number |  | ≥ 0, ≤ 1 |  |
| `offsetDeg` | number |  | ≥ -180, ≤ 180 |  |
| `driftMs` | number |  | ≥ 0 |  |

#### scoring.stars[]

| Field | Type | Required | Limits | Description |
|-------|------|----------|--------|-------------|
| `maxTimeS` | number |  | > 0 |  |
| `maxErrorM` | number |  | > 0 |  |
| `minBatteryPct` | number |  | ≥ 0, ≤ 100 |  |
| `minScore` | number |  | ≥ 0 |  |

<!-- END GENERATED: mission-reference -->
