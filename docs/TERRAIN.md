# Terrain guide

Drone Simulator flies over three kinds of terrain:

- **procedural** terrain, generated from a seed (the default);
- **elevation files** you provide: rasters (COG / GeoTIFF, `.hgt`, `.asc`, `.img`) or
  classified lidar point clouds (LAS / LAZ);
- **online elevation** from a web service such as OpenTopography or the French
  Géoplateforme (cartes.gouv.fr).

File and online terrain can be covered with **aerial imagery**, either from orthophoto files
or streamed from an imagery provider.

Choose the terrain in the **Terrain** tab, or in a mission file (see
[MISSIONS.md](MISSIONS.md) for the mission keys). This guide explains how to prepare the
data. The full requirements are in [SPECIFICATION.md](SPECIFICATION.md) §4.5.

## Procedural terrain

Seeded fractal noise. The same seed and parameters always give the same terrain.

| Parameter | Range | Default | Effect |
|---|---|---|---|
| `seed` | any integer | 1 | Which terrain. |
| `octaves` | 1–16 | 7 | Detail levels. |
| `featureSizeM` | m | 2000 | Wavelength of the largest hills. |
| `lacunarity` | 1.5–4 | 2 | Frequency step between octaves. |
| `persistence` | 0.05–0.95 | 0.5 | Amplitude step between octaves (rougher when higher). |
| `amplitudeM` | 0–8000 | 250 | Height of the hills. |
| `baseHeightM` | m | 0 | Offset of the whole surface. |
| `mountains` | 0–1 | 0.35 | Ridged mountains. |
| `warp` | 0–1 | 0 | Domain warping: twisted, less regular shapes. |
| `erosion` | 0–1 | 0.3 | Smoother valleys, sharper peaks. |
| `waterLevelM` | m | none | Lakes and sea below this height. |
| `snowLineM` | m | from the height range | Snow above this height. |
| `flatZones` | up to 64 | none | Level discs (`x`, `z`, `radiusM`, `blendM`), e.g. for pads. |

The built-in presets are *Rolling hills* (the default), *Alpine valley*, *Flat farmland*,
*Island (10 × 10 km)* and *Coast with lakes*. A mission names a preset in `terrain.preset`
and overrides single parameters in `terrain.params`. Bounded terrain (`extent`) ends in a
*fade* or a *cliff* edge. Details below 4 m are not generated.

## Elevation rasters

| Format | Notes |
|---|---|
| COG / GeoTIFF (`.tif`) | Best choice. Int16, UInt16, Int32, Float32 or Float64. |
| SRTM (`.hgt`) | WGS 84 implied. |
| ESRI ASCII grid (`.asc`) | Needs its `.prj` beside it, or `crs` in the mission. |
| ERDAS Imagine (`.img`) | |

- **Several tiles** of one CRS form one surface (a mosaic). Tiles in different CRSs are
  refused: reproject them first.
- **CRS:** taken from the file. Files without one need `crs` (for example `"EPSG:25830"`).
  Projected (UTM, national grids) and geographic CRSs both work. The heights are reprojected
  into the scene's local east-north-up frame, so no conversion is needed for that.
- **Units:** metres and feet (international or US survey) are converted.
- **NoData:** small holes are filled from their surroundings (within 8 pixels), larger ones
  take the lowest height of the data.
- **Size:** at most 40 000 px a side and 4 GB per terrain.
- **Levels of detail:** files with overviews load faster when you fly high; a COG has them.
- **Terrain size:** in the Terrain tab, *Dimensions › Fit to the data* (the default for
  files) makes the terrain cover the data: all of it when the origin is the centre of the
  data, otherwise the largest rectangle centred on the origin. The terrain is aligned with
  true north, so over a projected grid a few metres at the edges may be left out. The
  width × length then show the size; *Custom* starts from it, to fly over a part. The same
  applies to LAS/LAZ point clouds, once their terrain model is made.

Making a COG with overviews, in the CRS of your choice, from any raster GDAL reads:

```sh
# Reproject to UTM 30N at 2 m and write a COG (DEFLATE, overviews included).
gdalwarp -t_srs EPSG:25830 -tr 2 2 -r bilinear -of COG -co COMPRESS=DEFLATE \
    -co PREDICTOR=FLOATING_POINT input.tif dem/area.tif

# Join tiles into one COG.
gdalbuildvrt tiles.vrt tiles/*.tif
gdal_translate -of COG -co COMPRESS=DEFLATE tiles.vrt dem/area.tif
```

## Interpolation and smoothing

The *Elevation processing* group of the Terrain tab changes how heights are made from file
and online elevation data (rasters, lidar terrain models and providers). It is not shown
for procedural terrain. Press *Regenerate* to apply it. The mesh, the physics, the drone's
height above ground and the 2D preview all use the same heights.

The steps run in this order: NoData holes are filled, spikes removed, the cells smoothed,
then heights interpolated between the cells.

| Option | Choices | Use it for |
|---|---|---|
| *Interpolation* | **Bilinear** (default): straight lines between cell centres. | Most data. On coarse data (25–30 m) slopes show creases along the cells. |
| | **Bicubic** (Catmull-Rom): smooth slopes that still pass through every cell height. | Coarse DEMs (SRTM, Copernicus, BD ALTI). It can overshoot by a little at cliffs. |
| | **B-spline**: the smoothest surface. It does not pass through the cells, so peaks and ridges come out slightly lower. | Noisy or coarse data where a soft look matters more than exact heights. |
| | **Nearest**: each cell is a flat step. | Seeing the raw cells of the data. |
| *Remove spikes* | Median filter radius, 0 (*Off*) to 10 m. | Isolated spikes and pits: lidar noise, birds, artefacts at the seams of DEM tiles. Slopes and edges stay sharp. |
| *Smoothing* | Gaussian radius, 0 (*Off*) to 100 m. The kernel reaches the radius (σ is half of it). | Noise, and the terracing of data stored in whole metres. It also softens small real detail. |

- **Radius and cells:** a filter always covers at least the neighbouring cells. On fine
  data a wide filter would need very many cells, so it reads coarser, averaged cells. Above
  6 cells for spike removal or 24 cells for smoothing, the cell size doubles until the
  filter fits. For example, smoothing over 100 m on 1 m lidar uses 8 m cells, which is
  smooth anyway.
- **Cost:** the filtered cells are computed once per 128-cell tile and kept in a 64 MB
  cache, so the terrain loads slightly slower but flies at the same speed.
- **Online streaming:** with a filter on, each streamed block is fetched with a wider margin
  (about 1.25 × the filter reach), so blocks join without seams. The first flight after
  turning a filter on downloads the blocks again. A bounded online area already has a 300 m
  margin, so it needs no new download.
- **Saved with the scene and presets** as a `processing` object, for example:

  ```json
  "processing": { "interpolation": "bicubic", "despikeRadiusM": 2, "smoothRadiusM": 10 }
  ```

  Missions always use the default (bilinear, no filters), so every pilot flies the same
  surface.
- **Metadata:** the terrain information in the Terrain tab shows a line such as
  *Heights: bicubic, spikes removed within 2 m, smoothed over 10 m*.

## Mesh detail

*Finest mesh spacing* in the Terrain tab (any terrain) sets the distance between the
vertices of the most detailed mesh, which is drawn near the camera and also used by the
physics: 0.25, 0.5, 1, **2 (default)** or 4 m. Press *Regenerate* to apply it.

- **When to change it:** with elevation data finer than 2 m (lidar terrain models, 0.5 m or
  1 m DEMs), a spacing near the data's cell size shows its detail: banks, ditches, kerbs,
  rocks. A spacing finer than the data adds vertices but no detail. 4 m lightens the load
  on slow computers.
- **Only near the camera:** each level of detail is drawn at a distance proportional to its
  size, so a finer spacing adds levels close to the camera and leaves distant terrain as
  it was. Each halving draws about 15 % more terrain meshes and imagery tiles low over
  the ground, and none from about 1000 m up.
- **Imagery** gets finer near the camera with the mesh, so draped orthophotos show their
  full resolution close up.
- **Saved with the scene and presets** as `"meshSpacingM": 0.5` (left out at 2 m).
  Missions always use 2 m, so every pilot flies the same surface.

## Lidar point clouds (LAS / LAZ)

LAS 1.0–1.4 and LAZ, including COPC files, are turned into a terrain model (a DTM) once and
cached. Building it takes about 10 s per 40 million points (release build).

- **Classification:** only **ground (class 2)** and **water (class 9)** points shape the
  surface. Withheld points and noise (classes 7 and 18) are dropped. Buildings and
  vegetation are left out, which gives a DTM (bare earth), not a DSM.
- **Unclassified clouds** are refused with a message. Automatic ground classification
  (`classifyGround`) is not available yet. Classify the cloud first, for example with PDAL:

  ```sh
  pdal translate raw.laz classified.laz smrf
  ```

- **Resolution** (`resolutionM`, at least 0.25 m): by default about two point spacings,
  between 0.5 and 20 m, and coarser when the grid would exceed 32 million cells. In the
  Terrain tab, untick *DTM cell size › Automatic* to set it. To see the detail of a fine
  model, also set *Finest mesh spacing* (see *Mesh detail*) to about its cell size.
- **Cell height:** the mean of the ground points in a cell (the default, the least noise),
  or the lowest of them (*Cell height › Lowest ground point* in the Terrain tab,
  `"cellHeight": "lowest"` in a saved scene). The lowest point drops low vegetation and
  objects wrongly classified as ground, but follows the low side of any noise. Choosing it
  makes a new terrain model. *Remove spikes* (see above) then cleans up what is left.
- **Gaps** up to 50 m across are filled by triangulation (`"interpolation": "tin"`, the
  default) or inverse-distance weighting (`"idw"`). Larger gaps keep no height and are filled
  like raster NoData.
- **CRS:** from the WKT or GeoTIFF records of the file, else `crs`. Geographic CRSs are refused
  (lidar is always projected).
- **Size:** at most 20 GB per terrain.
- **Cache:** the DTM is a COG in `~/.drone_simulation/cache/dem/`, named after the files'
  paths, sizes and dates and the settings, so a changed file is rasterized again. *Clear
  Cache* in the settings removes it.

## Orthophotos (aerial imagery files)

Orthophotos are draped over file or online elevation and lit like the terrain. Where there
is no image, the usual terrain materials show, blended softly at the image edge.

- **Formats:** 8-bit COG / GeoTIFF, JPEG 2000 (`.jp2`), or PNG / JPEG with a world file
  (`.pgw`, `.jgw`) and `orthophotoCrs`. Bands: grey, grey + alpha, RGB or RGBA, or pick them
  with `orthophotoBands` (for example `[1, 2, 3]` for a four-band RGB + NIR image). A NoData
  value makes those pixels transparent.
- **Several images** of one CRS form a mosaic. Any CRS works: each tile is reprojected as it
  is drawn.
- **Large images:** images over 4096 px without overviews are converted once into a cached COG
  (`~/.drone_simulation/cache/ortho/`). Add overviews yourself to skip this:

  ```sh
  gdal_translate -of COG -co COMPRESS=JPEG -co QUALITY=85 ortho.tif ortho/area.tif
  # or keep the file and add overviews in place
  gdaladdo -r average ortho.tif
  ```

- **Colour:** `orthophotoColor` adjusts `brightness` (−1 … 1), `contrast` (0 … 4) and `gamma`
  (0.1 … 10).
- **Size:** at most 200 000 px a side.
- **Failures** never stop a mission: a missing or damaged image only disables the imagery,
  and the status bar says why. Orthophotos are not part of the terrain data hash.

## Imagery providers

Instead of files, imagery can be streamed from a web map service while you fly (Settings ›
Data providers). Supported services are Web-Mercator tile services (EPSG:3857):

| Protocol | Template placeholders | Example |
|---|---|---|
| `xyz` | `{z}`, `{x}`, `{y}` | `https://tiles.example.org/{z}/{x}/{y}.jpg` |
| `tms` | as xyz, rows counted from the south | |
| `wmts` | as xyz (`TileMatrix={z}&TileRow={y}&TileCol={x}`) | `GoogleMapsCompatible` matrix set, or one with the same grid (the Géoplateforme's `PM_0_19`) |
| `wms` | `{bbox}` (EPSG:3857 metres), `{width}`, `{height}` | `…&CRS=EPSG:3857&BBOX={bbox}&WIDTH={width}&HEIGHT={height}` |

The built-in providers need no key:

| Provider | Key | Layers | Area |
|---|---|---|---|
| IGN PNOA | `ign-pnoa` | orthophotos | Spain |
| swisstopo SWISSIMAGE | `swisstopo-swissimage` | orthophotos | Switzerland |
| USGS The National Map | `usgs-imagery` | orthophotos | USA |
| Géoplateforme (IGN France, cartes.gouv.fr) | `geoplateforme-imagery` | `ORTHOIMAGERY.ORTHOPHOTOS` (BD ORTHO aerial photographs), `GEOGRAPHICALGRIDSYSTEMS.PLANIGNV2` (Plan IGN map) | metropolitan France and Corsica |
| OpenStreetMap | `osm-standard` | the standard map | world |

Imagery is not downloaded ahead of a flight: what you have flown over stays in the cache and
is used offline, for as long as the provider allows.

**OpenStreetMap** draws a street map, not a photograph, on the terrain: useful to find roads,
paths and buildings. Its tile servers are run by volunteers on donated resources, and their
[tile usage policy](https://operations.osmfoundation.org/policies/tiles/) applies:

- tiles are fetched only for the area you are flying over, two at a time, and never in bulk;
- the simulator honours the servers' cache lifetime (at least 7 days), and it never uses
  expired tiles offline, which the policy forbids (`"serveStale": false`): without network,
  only recently fetched tiles are shown;
- the attribution "© OpenStreetMap contributors" is shown in the view;
- heavy use can be blocked by the OpenStreetMap operators without notice. For a lot of
  flying, add your own tile server or a commercial OpenStreetMap tile service in Settings ›
  Data providers.

## Mission folders and paths

A mission with its own data lives in a folder:

```
my-valley/
├── my-valley.mission.json
├── dem/
│   ├── tile-1.tif
│   └── tile-2.tif
└── ortho/
    └── valley.tif
```

- Paths are relative to the mission folder and use `/`. Absolute paths, `..` and symbolic
  links leading outside the folder are refused.
- `library:<path>` refers to the **geodata library** `~/.drone_simulation/library/`, for data
  shared by several missions.
- A `sha256` per file is optional. When it is given, a different elevation file stops the
  mission, and a different image only disables the imagery.
- A path ending in `/` (for example `library:my-lidar/`) stands for every file of the
  mission's format directly in that folder, sorted by name, so the files may have any names.
  It takes no `sha256`. Subfolders, other file types and symbolic links leading out of the
  folder are ignored.
- A missing elevation file, or a folder without such files, makes the mission
  *unavailable*, with the reason in the Missions tab.
- Without an `extent`, the terrain covers what the data covers; a spawn or item outside it
  stops the launch with its distance from the origin.

## Built-in real-terrain missions

The real-terrain chapter has missions on real data. The data is published by mapping
agencies and the Copernicus programme under licences that allow redistribution with
attribution, but it is large, so the simulator does not ship it.

| Mission | Data | Download |
|---|---|---|
| *Montserrat ridge* | OpenTopography COP30, online (needs your API key) | at launch |
| *Oeschinensee* | swisstopo swissALTI3D 2 m, 8 GeoTIFF tiles | 8 MB |
| *Sau reservoir* | Copernicus DEM GLO-30 (COG) and a PNOA orthophoto (JPEG + world file) | 25 MB |

Fetch the data once into the geodata library with `fetch_mission_data.py`. It is in `scripts/data/` in
the source tree and in `/opt/drone_simulator/share/scripts/` when installed, and it needs
only Python 3:

```sh
python3 /opt/drone_simulator/share/scripts/fetch_mission_data.py           # everything
python3 fetch_mission_data.py --list                  # what is there, licences
python3 fetch_mission_data.py --mission real-03-sau   # one mission
python3 fetch_mission_data.py --library /data/geo     # another library folder
```

The script shows the licences and the size and asks before downloading. It checks every
published file against its size and SHA-256, writes through temporary `.part` files, and
leaves an `ATTRIBUTION.txt` beside the data. Run it again to resume. The missions become
available after *Reload* in the Missions tab, or at the next start.

The two tours waive the line-of-sight rule, because they go beyond what a pilot at the
take-off point could see. In reality you would need observers.

### Your own lidar

*Your own lidar* (`real-04-own-lidar`) ships and downloads nothing: it flies over the point
cloud you provide. Put one or more LAS or LAZ files into `~/.drone_simulation/library/my-lidar/`
(any names), then press *Reload* in the Missions tab.

- The files must be **classified**, with ground in class 2, and share one projected CRS.
- Together they must cover at least **260 × 260 m** around their centre. The take-off pad
  is at the centre of the data; four beacons stand 30 m above the ground, 100 m east or west
  and 100 m north or south of it.
- Files without a CRS cannot be used as they are: copy the mission into
  `~/.drone_simulation/missions/` and add `"crs"` to its `terrain`.
- The terrain model is made at the first launch and cached, so later launches are quick.
- The clock is 14:00 UTC in June. In Asia and Oceania the flight is at night: switch the
  vehicle lights on to stay in sight.

## Online elevation providers

Online elevation comes from REST services that return a raster for a bounding box
(`"protocol": "rest_bbox"`). Providers are JSON files (the built-in ones in
`share/providers/`) or are added in Settings › Data providers. The main keys:

| Key | Meaning |
|---|---|
| `key`, `name` | Stable id and display name. |
| `kind` | `elevation` or `imagery`. |
| `protocol` | `rest_bbox` (elevation); `xyz`, `tms`, `wmts`, `wms` (imagery). |
| `urlTemplate` | Request URL with placeholders: `{dataset}`, `{south}`, `{north}`, `{west}`, `{east}`, `{bbox}`, `{width}`, `{height}`, `{crs}`, `{format}`, `{apiKey}`, `{z}`, `{x}`, `{y}`. |
| `format`, `crs` | Requested format and CRS (`GTiff`, EPSG:4326 for elevation; `image/jpeg`, EPSG:3857 for imagery). |
| `datasets` | Layers: `id`, `name`, `resolutionM`, `maxAreaKm2`, `verticalDatum`, `coverage`, `attribution`; imagery layers add `minZoom` / `maxZoom`. |
| `requiresKey`, `apiKey` | Whether a key is needed and where it goes (query parameter or header). |
| `attribution`, `licence`, `termsUrl` | Shown with the terrain and in the settings. |
| `allowCache`, `maxAgeDays` | Caching allowed by the terms, and for how long. |
| `rateLimitRps`, `dailyCallLimit`, `maxConcurrent` | Request limits. |
| `maxResponseBytes`, `contentTypes`, `quotaErrorPatterns` | Response checks. |
| `maxSizePx` | Largest image the server makes (a WMS's `MaxWidth`); larger areas are split. |
| `serveStale` | `false` if expired cache entries may not stand in offline (default `true`). |
| `blockSizeKm` | Side of the blocks streamed around the drone. |

**OpenTopography** (built in): create a free account at
<https://portal.opentopography.org/>, request an API key, and enter it in Settings › Data
providers. It is stored in the system keyring (or in the private database if there is no
keyring). It can also come from the environment variable `DS_API_KEY_OPENTOPOGRAPHY`.
Non-academic keys allow **50 calls per 24 hours**. The simulator counts them, shows what is
left, and asks before a download. *Test connection* uses one call.

**Géoplateforme** (built in, `geoplateforme-elevation`): the French mapping agency IGN's
elevation service on cartes.gouv.fr, free and without a key. It covers metropolitan France and
Corsica with three layers, all heights in NGF-IGN69 (IGN78 in Corsica):

| Dataset | Resolution | |
|---|---|---|
| `ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES` | 5 m | RGE ALTI, the default |
| `IGNF_LIDAR-HD_MNT_ELEVATION.ELEVATIONGRIDCOVERAGE.WGS84G` | 1 m | terrain model from the LiDAR HD survey; parts not yet surveyed have no data |
| `ELEVATION.ELEVATIONGRIDCOVERAGE` | 25 m | BD ALTI |

The data is under the Licence Ouverte 2.0 (Etalab): free use, including commercial use, with
the source named; the attribution is shown in the view. The service allows 40 requests per
second and images of up to 5010 px a side, so the simulator asks for at most 5000 px and
streams 4 km blocks.

- **Areas:** a bounded online terrain (an online mission defaults to 10 × 10 km) is downloaded
  once, in as few requests as the provider's limits allow, before the flight. It is then
  pinned in the cache, so the mission plays offline and records the same data hash.
- **Streaming:** an unbounded online terrain loads blocks around the camera as you fly.

## CRS and vertical datums

- The scene uses a local east-north-up frame at the terrain origin (by default the centre of
  the data). Every source is reprojected into it.
- An origin with `"altRef": "AGL"` (or none) puts the ground at the origin at height 0. An
  `"AMSL"` origin altitude becomes height 0.
- Heights are used as the data gives them. The vertical datum (from the file, the provider's
  dataset or the mission's `verticalDatum`, e.g. `EGM2008`, `LN02`) is shown in the metadata
  and the HUD's AMSL elevation, but it is not converted. Ellipsoidal heights (such as the
  OpenTopography `_E` datasets) differ from sea-level heights by tens of metres, so prefer
  orthometric data.

## Cache and offline use

Everything downloaded or derived lives in `~/.drone_simulation/`:

| Path | Content |
|---|---|
| `cache.db`, `cache/` | Provider responses (elevation areas and blocks, imagery tiles). |
| `cache/dem/` | DTMs rasterized from point clouds. |
| `cache/ortho/` | COG copies of large orthophotos. |
| `library/` | The geodata library (yours, and the built-in missions' data). |

Settings › Cache shows the usage per provider, sets the size limit (5 GB by default), clears
the cache, and switches **offline mode**. Offline, only cached data is used: missions on file
data, downloaded online areas and imagery already seen work as before. Pinned online areas
are kept until you clear the cache. The geodata library is never touched by the cache.
