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

### Terrain from elevation files

A mission can fly over real terrain from **elevation rasters**: COG / GeoTIFF, SRTM `.hgt`,
ESRI ASCII grid `.asc` (with its `.prj`) or ERDAS Imagine `.img`, or **classified lidar point
clouds** in LAS 1.0–1.4 or LAZ (see *Terrain from lidar point clouds* below).

- **Where the files are:** put the mission in a folder of its own, with the files next to it
  (usually in `dem/`). Paths are relative to that folder, or `library:<name>` for the geodata
  library `~/.drone_simulation/library/`. A missing file makes the mission *unavailable*.
- **Folders:** a path ending in `/`, such as `"library:my-lidar/"`, stands for every file of
  the mission's format directly in that folder, in name order, under any names. Use it for
  data the user provides. It takes no `sha256`, and an empty folder makes the mission
  unavailable.
- **Several tiles** are joined into one surface. They must share one CRS. Files without a
  CRS need `crs`, e.g. `"EPSG:25830"`.
- **Origin:** without `origin`, the origin is the centre of the data and the ground there is
  at height 0. With an `origin` whose `altRef` is `"AMSL"`, that altitude is height 0.
- **Extent:** without `extent`, the terrain is the largest rectangle centred on the origin
  that the data covers, and a spawn or item outside it stops the launch. With `extent`, the
  rectangle must lie inside the data. A HUD caution
  appears 300 m from the edge and a warning 100 m from it.
- **Checks at launch:** the files are hashed, behind a progress dialog for large files.
  A file whose hash differs from its `sha256` stops the mission. The attempt records the hash
  of the data and of the settings that change heights (`terrain_data_hash`), and the personal
  best of the debrief only counts attempts flown on the same data.
- **Attribution:** `attribution` is shown at the bottom right of the view.

### Orthophotos

File terrain can be covered with aerial imagery, listed in `orthophotoFiles` with the same
path rules as the elevation files (usually in `ortho/`):

```json
"orthophotoFiles": [{ "path": "ortho/area.tif", "sha256": "…" }],
"orthophotoAttribution": "PNOA © IGN, CC BY 4.0",
"orthophotoColor": { "brightness": 0.05, "contrast": 1.1, "gamma": 1.0 }
```

- **Formats:** 8-bit COG / GeoTIFF, JPEG 2000 (`.jp2`), or PNG / JPEG with a world file
  (`.pgw`, `.jgw`) plus `orthophotoCrs`. Several files are joined into one mosaic, and they
  must share one CRS. `orthophotoBands` picks the red, green, blue (and alpha) bands of
  multi-band images, e.g. `[1, 2, 3]`.
- **Coverage:** areas outside the images, transparent areas and NoData show the usual
  terrain materials, blended softly at the edge.
- **Large files:** images over 4096 px without overviews are converted once to a cached COG
  in `~/.drone_simulation/cache/ortho/`, behind a progress dialog. Add overviews to your
  files (`gdaladdo`) to skip this.
- **Problems only disable the imagery:** a missing or damaged image, or a `sha256` that does
  not match, still lets the mission fly without imagery, and the status bar says why. The
  images are not part of `terrain_data_hash`.
- **Attribution:** shown after the elevation attribution, separated by "·".

### Imagery from a provider

Instead of `orthophotoFiles`, file and online terrain can be covered with imagery from an
**imagery provider** (Settings › Data providers), streamed while you fly:

```json
"imageryProvider": "ign-pnoa",
"imageryDataset": "OI.OrthoimageCoverage"
```

- **Providers:** the built-in ones are IGN PNOA (Spain), swisstopo SWISSIMAGE
  (Switzerland) and USGS The National Map (USA); none needs a key. You can add others that
  serve Web-Mercator tiles (XYZ, TMS, WMTS `GoogleMapsCompatible`) or EPSG:3857 WMS maps.
  `imageryDataset` picks a layer; without it the provider's default layer is used.
- **Files or a provider:** a mission gives `orthophotoFiles` or `imageryProvider`, not both.
- **Unknown providers** make the mission invalid. A provider that needs an API key you have
  not entered, or that has no imagery at the origin, only disables the imagery, and the
  status bar says why.
- **Offline:** imagery is not downloaded before the flight. What you have flown over is kept
  in the cache, and is used when you are offline; elsewhere the terrain materials show.
- **Not scored:** imagery is not part of `terrain_data_hash`.
- **Attribution:** the provider's and the layer's attribution are shown after the elevation
  attribution.

### Terrain from lidar point clouds

With `"format": "las"` or `"laz"`, the `elevationFiles` are point clouds. They are turned
into a bare-earth elevation model (DTM) the first time the mission starts. A progress dialog
shows the work and has a *Cancel* button. The DTM is then kept in the cache
(`~/.drone_simulation/cache/dem/`), so later starts are immediate.

```json
"terrain": {
  "source": "file",
  "format": "laz",
  "elevationFiles": [{ "path": "lidar/tile_1.laz" }, { "path": "lidar/tile_2.laz" }],
  "resolutionM": 1.0,
  "interpolation": "tin",
  "attribution": "Lidar © National Mapping Agency, CC BY 4.0"
}
```

- **The cloud must be classified.** Ground points (class 2) give the surface, together with
  water (class 9). Vegetation, buildings and the other classes are ignored. Noise (classes 7
  and 18) and points flagged *withheld* are dropped. A cloud with no ground points stops the
  mission with a message. Classify it first, for example with PDAL's SMRF filter or LAStools'
  `lasground`. `"classifyGround": true` is accepted, but automatic classification is not
  available yet.
- **CRS:** taken from the files (GeoTIFF keys or WKT). Files without one need `crs`. The CRS
  must be projected (UTM or a national grid); geographic coordinates are rejected. Heights
  stored in feet are converted to metres.
- **`resolutionM`** is the cell size of the DTM, 0.25 to 100 m. Without it, the cell size is
  about two point spacings (0.5 to 20 m). A grid of more than 32 million cells is refused:
  use a coarser `resolutionM` or fewer tiles.
- **`interpolation`** fills gaps without ground points, such as under buildings or over
  water. `"tin"` (the default) triangulates the edge of the gap; `"idw"` uses inverse distance
  weighting. Gaps up to 50 m across are filled. Larger gaps, and areas outside the data, stay
  without data and are filled as for rasters.
- **Several tiles** are joined into one DTM and must share one CRS.
- A changed file or a changed setting makes a new DTM. *Settings › Data providers › Clear
  Cache* removes the DTMs; they are made again when needed.

### Terrain from an online provider

With `"source": "online"`, the terrain is downloaded from an elevation provider, first
**OpenTopography**: `elevationProvider` is the provider key (`"opentopography"`), `dataset`
its dataset (e.g. `"COP30"`, else the provider's default). `origin` is required.

- **Area:** the terrain is the `extent` around the origin, or 10 km × 10 km without one. It is
  downloaded when the mission starts, after a question showing the number of calls and how
  many are left today. From then on it is in the cache and the mission also works offline.
- **API key:** OpenTopography needs a free personal key. Enter it in Settings › Data
  providers, which keeps it in the system keyring. The environment variable
  `DS_API_KEY_OPENTOPOGRAPHY=<your key>` overrides the stored key. Without a key a mission is
  *unavailable* unless its area is already cached.
- **Your own provider:** `elevationProvider` may also be the key of a provider added in
  Settings › Data providers.
- **Origin:** as for files: with `altRef` `"AMSL"` that altitude is height 0, otherwise the
  ground at the origin is.
- **Scores:** the attempt records the hash of the downloaded data; personal bests only compare
  attempts on the same data.

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
| `parkingZone` | box `sizeM` [w, l] (2 × 3 m), `heightM` 2, rotated by `headingDeg` | Target heading for `park`. A bay painted on the ground (white lines, yellow heading mark): always drawn, like a landing pad. Size it for the vehicle (0.7 × 0.9 m for the 4WD rover). |
| `inspectionTarget` | sphere, `radiusM` 0.5 | |
| `surveyArea` | prism from `polygon`, or a cylinder | |
| `noFlyZone` | keep-out prism (`polygon`) or cylinder (`radiusM`), `bottomM`/`topM` optional | Entering it fails the mission. |
| `geofence` | keep-in prism or cylinder | Leaving every geofence fails the mission. |
| `obstacle` | `shape`: `box` (`sizeM` [w, h, l]), `cylinder`, `tree`, `building`, `powerLine`, `windTurbine`, `house`, `cow`, `man` (see below) | Solid: blocks flight and the pilot's line of sight. |
| `ramp` | wooden wedge `sizeM` [w, h, l] (1 × 0.25 × 1.5 m) rising towards `headingDeg`, vertical face at the high end | Drivable (ground drones drive up it and jump off the end); solid for aerial drones. |
| `bridge` | concrete deck, top `h` above the position over `l` (`sizeM` 1 × 0.5 × 3 m, 0.3 m thick), approach ramps of `rampLengthM` (12° slope) at both ends along `headingDeg`, yellow kerbs unless `kerbs` is false | Drivable on the deck and ramps; room to pass under a deck higher than 0.3 m. |
| `payload` | small box | |

### Obstacle figures

Houses, trees, cows and people are made of several solid parts, and the drone collides with
the parts, not with a box around them. You can fly under the eaves of a house or the crown of
a tree, or between a cow's legs. Each part has a material that sets how the drone bounces off it
(`restitution`) and slides along it (`friction`). Each figure also has a typical mass that
scales with its size. Obstacles never move, but a light one takes part of the impact: a 25 kg
drone bounces off a person less than off a wall.

| `shape` | Size (defaults) | Parts and materials | Typical mass |
|---------|-----------------|---------------------|--------------|
| `house` | `sizeM` [8, 7, 10] (width, height, length) | masonry walls (62 % of the height), tiled gable roof along the length with 0.3 m eaves, chimney | 250 kg/m³ of the box: 140 t |
| `tree` | `radiusM` 2 (crown), `heightM` 8 | wooden trunk, two foliage spheres | wood 700 kg/m³, crown 10 kg/m³: 1 t |
| `cow` | `sizeM` [0.8, 1.5, 2.4] | hide: body, head (towards −z, the heading), four legs | 650 kg |
| `man` | `heightM` 1.75 (the rest scales) | clothing: legs, torso, arms; skin: neck, head | 75 kg × (h / 1.75)³ |

| Material | Restitution | Friction |
|----------|:-----------:|:--------:|
| concrete (boxes, masts, gate frames) | 0.2 | 0.5 |
| masonry (walls, buildings) | 0.2 | 0.6 |
| roof tiles | 0.25 | 0.5 |
| wood (trunks) | 0.25 | 0.6 |
| foliage (crowns) | 0.05 | 0.9 |
| hide, skin | 0.1 | 0.6 |
| clothing | 0.1 | 0.7 |

`massKg`, `restitution` and `friction` on an obstacle replace these values for the whole item.

**Model files.** `"model": "crate.obj"` draws an obstacle from a mesh file instead of its
shape: glTF 2.0 (`.gltf`, `.glb`) or Wavefront OBJ (`.obj` with its `.mtl`), in the glTF axes
(+Y up, front towards +Z). The model is scaled to fit the shape and rests on the ground; the
shape still decides collisions and line of sight, so size the shape (`sizeM`, …) to match.
Colours come from the materials (texture maps are ignored). The file is looked for in the
mission's folder first, then in `~/.drone_simulation/models/` and the built-in models
(`crate.obj`); `..` is not allowed. A missing file draws the shape and logs a warning.

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
failure starts, lasting `durationS` seconds (until the end of the attempt without it). The
event log shows it; the HUD shows only what the pilot would notice (no GPS fix, the failsafe).

| `type` | Effect | Parameters (default) |
|--------|--------|----------------------|
| `gpsDrift` | The GPS position walks away from the truth in a random direction; position hold and RTH follow it | `driftMs` (0.5), `magnitudeM` (20) |
| `gpsLoss` | No GPS fix: position hold flies as altitude hold, RTH is unavailable | — |
| `motorFailure` | One motor keeps a share of its thrust (a ground drone's wheel motor, of its drive); a quad loses control with a dead motor | `motor` 1–4 for a quad (1), `efficiency` 0–1 (0) |
| `batteryDrop` | A cell fault: a share of the rated capacity is lost at once | `fraction` (0.3) |
| `compassError` | Magnetic interference: the heading is off, position hold circles or flies away | `offsetDeg` (30) |
| `rcLoss` | The RC link is lost: the failsafe returns home (with GPS) or lands | — |
| `baroDrift` | The barometric altitude walks away: altitude hold climbs or sinks | `driftMs` (0.2), `magnitudeM` (15) |

## Weather

`environment.weather` drives a **seeded weather generator** (FR-WX-01…03): the same seed and
settings always give the same weather.

1. The conditions start from the defaults: 15 °C, QNH 1013.25 hPa, 50 % humidity, calm, clear
   sky, 50 km visibility.
2. The **preset** sets the sky (from `assets/environment/weather_presets.json`):

   | Preset | Clouds | Precipitation | Visibility | Also sets |
   |--------|--------|---------------|-----------:|-----------|
   | `clear` | none | none | 50 km | |
   | `few-clouds` | cumulus 2/8 at 1200–1800 m | none | 40 km | |
   | `scattered-clouds` | cumulus 4/8 at 1000–2000 m | none | 30 km | |
   | `broken-clouds` | stratocumulus 6/8 at 900–1500 m | none | 25 km | |
   | `overcast` | stratus 8/8 at 600–1200 m | none | 20 km | humidity 80 % |
   | `mist` | none | none | 3 km, mist up to 200 m | humidity 93 % |
   | `fog` | none | none | 500 m, fog up to 120 m | humidity 99 % |
   | `drizzle` | stratus 8/8 at 300–900 m | moderate drizzle (0.3 mm/h) | 8 km | humidity 95 % |
   | `light-rain` | stratus 8/8 at 700–2500 m | light rain (1 mm/h) | 10 km | humidity 88 % |
   | `heavy-rain` | stratus 8/8 at 500–3000 m | heavy rain (12 mm/h) | 3 km | humidity 95 % |
   | `light-snow` | stratus 8/8 at 800–2500 m | light snow (0.5 mm/h water) | 4 km | −2 °C, 90 % |
   | `heavy-snow` | stratus 8/8 at 500–3000 m | heavy snow (4 mm/h water) | 1 km | −4 °C, 95 % |
   | `thunderstorm` | cumulonimbus 6/8 at 900–11000 m, cumulus 3/8 | rain 20 mm/h | 5 km | 90 %, thunderstorm |

3. The other members **override** the preset: `temperatureC`, `humidityPct` (or `dewPointC`),
   `pressureHpa`, `wind` (`level`, `fromDeg`; default from 270°), `clouds` (replaces every
   layer; a layer without `topM` gets a thickness typical of its type), `precipitation`
   (`type` with `intensity` — light / moderate / heavy — or `rateMmH`), `visibilityM` and `fog`
   (`topM`: the visibility holds below it and the air is clear above; `haze`).
4. Each **timeline** entry starts at `atS` seconds after the vehicle is placed on the spawn
   point and blends over `transitionS` seconds (smoothly) into the previous conditions changed
   by its own `preset` and overrides. A preset in the timeline changes only what that preset
   sets: rain arriving keeps the temperature, pressure and wind of the mission unless the entry
   changes them. Wind levels blend through the intermediate levels.
5. Around these planned values the weather **fluctuates**: partial cloud cover wanders by up
   to ±0.75 okta (clear and overcast skies stay as they are) over a few minutes, the
   precipitation rate by ±40 % (and the visibility with it); the clouds drift with the wind
   at their height.

Inside a cloud layer of at least 4 oktas the visibility drops to 60 m, so the pilot loses
sight of a drone that climbs into the cloud (VLOS). Temperature, pressure and humidity set the
air density (thrust) and the battery temperature (cold packs hold less and sag more); the
barometer reads relative to the QNH. Rain wets the airframe of aerial drones (a few tens of
grams at most) and the ground: wet ground and lying snow take grip away from ground drones (a
slope that is easy when dry may be impossible in the rain). A mission that starts in rain
starts on wet ground; one that starts in snow at ≤ 1 °C starts with snow on the ground.

The 3D view draws the weather: the cloud layers drift with the wind and cast moving shadows;
an overcast greys the sky and takes the shadows away; fog and low visibility hide the
distance; rain streaks and snowflakes fall around the camera; wet ground looks darker and
shiny and lying snow turns it white. At night the clouds hide the moon and the stars. In the
FPV view, rain leaves drops on the lens until it dries; thunderstorms light the clouds with
flashes of lightning.

The **briefing** summarises the weather at the start, e.g. *Wind 270° level 4 (24 km/h,
gusts 34) · broken clouds at 900 m · light rain · visibility 6.0 km · 12 °C · QNH 1009*. The
timeline is listed only when `forecast` is true. During the flight the HUD shows the weather under the
wind in METAR style, e.g. *WX BKN 900 m -RA VIS 6 km 12 °C* (amber under 5 km visibility or in
precipitation, red under 1 km or in a thunderstorm). Outside missions the same generator is in
the Environment tab: preset, seed with *Randomise*, cloud layers, precipitation, visibility,
fog, thunderstorm and one timed change that can be switched off.

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
2. The panel hides and the drone waits on the take-off point. Where the ground slopes more
   than 5°, an aerial drone stands on a level **launch platform** marked with an "H". It
   serves for the take-off and as the landing spot: return to home lands back on it.
   **Arm** the drone (Space, or the arm switch of your transmitter): the clock starts. The HUD shows the mission panel (state,
   timer, objectives with the distance to each target) and markers on the current targets.
3. Rule warnings (height limit, distance, line of sight, no-fly zones, lights, …) appear in
   red; a rule still broken after the grace time (`rules.graceS`, 3 s by default) fails the
   mission. Trees, masts, buildings, houses, animals, people and gate frames are **solid**:
   touching one with spinning propellers, or hitting it hard, is a crash.
4. When the attempt ends — success, failure, or *Missions › Abort Mission* — it is saved at
   once with your progress. The **debrief** follows: stars, score breakdown, the path you
   flew, and your personal best. Choose **Retry**, **Next mission** or **Back to free flight**
   (your free-flight scene, vehicle and camera come back as they were). The attempt's flight
   is recorded with its telemetry and replay; the **History** tab lists it and can replay
   it (the mission must still be installed).

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
| Real terrain | Montserrat ridge | online terrain (needs an OpenTopography API key) |
| Real terrain | Oeschinensee | swissALTI3D tiles with swisstopo imagery; alpine lake, falls, pasture |
| Real terrain | Sau reservoir | Copernicus 30 m COG with a PNOA orthophoto; pier and cliff plateau |
| Real terrain | Your own lidar | your LAS/LAZ files in `library:my-lidar/`; a 200 m square |

The first mission of each chapter is open; each further one needs a star on the one before.
*Oeschinensee*, *Sau reservoir* and *Your own lidar* instead open after a star on *Hover*.
No data is shipped for them: fetch the data of the first two once with
`fetch_mission_data.py`, and put your own point cloud into the library for the third
([TERRAIN.md](TERRAIN.md#built-in-real-terrain-missions)).

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
[`assets/missions/`](../assets/missions/) show every objective type. The `ground` chapter
(4WD rover, M6) has four missions: *First drive* (two markers, then park in the start bay),
*Tight parking* (between two parked rovers, 25 cm and 8°), *Sample run at dusk* (headlights
required, deliver a sample, park in the garage) and *Bridge course* (gates on the ground and
on a footbridge deck, a kicker ramp, a finish bay). Gates for ground vehicles sit with the
bottom of their opening at the driving surface (`altitude = heightM / 2 − 2 cm`, plus the
deck height on a bridge); put the `pilot` where the course is not hidden behind bridges or
buildings, or the line-of-sight rule fails the mission.

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
| `imageryProvider` | string \| null |  |  | Online imagery provider draped over the terrain (FR-TER-O10); null or absent ⇒ none. |
| `imageryDataset` | string |  | min length 1 | Layer of the imagery provider; absent ⇒ its default. |
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
| `orthophotoBands` | array of integer |  |  | Red, green, blue (and alpha) bands of multi-band images, e.g. [1, 2, 3]. |
| `orthophotoColor` | object |  |  | Colour adjustments of the orthophotos. |
| `imageryProvider` | string |  | min length 1 | Online imagery provider instead of orthophotoFiles (FR-TER-O10). |
| `imageryDataset` | string |  | min length 1 | Layer of the imagery provider; absent ⇒ its default. |

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
| `path` | string | yes | min length 1, max length 1024 | Relative to the mission folder, or prefixed with library: for the geodata library; a path ending in '/' stands for every file of the format directly in that folder (no sha256). |
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
| `shape` | "box" \| "cylinder" \| "tree" \| "building" \| "powerLine" \| "windTurbine" \| "house" \| "cow" \| "man" |  |  | Obstacle figure: house, tree, cow and man collide part by part (walls and roof, trunk and crown, body and legs). |
| `radiusM` | number |  | > 0 |  |
| `widthM` | number |  | > 0 |  |
| `heightM` | number |  | > 0 |  |
| `sizeM` | array of number |  | min items 2, max items 3 | [width, length] or [width, height, length] in metres. Ramps rise by the height towards their heading; a bridge's deck top is the height above its position, over the length. |
| `polygon` | array of array[2] |  | min items 3, max items 256 | Outline as [x, z] offsets in metres from the item position (engine frame: x east, z south). |
| `bottomM` | number |  |  |  |
| `topM` | number |  |  |  |
| `massKg` | number |  | > 0 | Payload mass; for obstacles, replaces the typical mass of the figure (a light obstacle takes part of a heavy drone's impact). |
| `restitution` | number |  | ≥ 0, ≤ 1 | Obstacles: fraction of the normal speed a drone keeps when it bounces off (default from the material: concrete 0.2, foliage 0.05, …). |
| `friction` | number |  | ≥ 0, ≤ 2 | Obstacles: Coulomb friction coefficient against the drone (default from the material: concrete 0.5, foliage 0.9, …). |
| `model` | string |  | max length 200, pattern `^[A-Za-z0-9_][A-Za-z0-9_./-]*\.([gG][lL][tT][fF]|[gG][lL][bB]|[oO][bB][jJ])$` | Obstacles: mesh file drawn instead of the shape (glTF 2.0 `.gltf`/`.glb` or `.obj`), looked for in the mission's folder, then `~/.drone_simulation/models/` and the built-in models; fitted to the shape, which still stops the vehicles. No `..`. |
| `order` | integer |  | ≥ 0 |  |
| `rampLengthM` | number |  | > 0 | Bridges: length of each approach ramp (default: a 12° slope up to the deck). |
| `kerbs` | boolean |  |  | Bridges: kerbs along both sides of the deck (default true). |

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
