# Third-Party Notices

Drone Simulator is released under the MIT License (see `LICENSE`).
It includes or uses the third-party components listed below.

## Included in the source tree and in the binary

### glad 2.0.8 — generated OpenGL loader (`third_party/glad/src/gl.c`, `include/glad/gl.h`)

- Generator: <https://github.com/Dav1dde/glad>
- Licence of the generated code: `(WTFPL OR CC0-1.0) AND Apache-2.0`.
  The Apache-2.0 part covers the OpenGL API registry (`gl.xml`) © The Khronos Group Inc.,
  from which the function declarations are generated.
  Apache License 2.0: <https://www.apache.org/licenses/LICENSE-2.0>

### khrplatform.h (`third_party/glad/include/KHR/khrplatform.h`)

```
Copyright (c) 2008-2018 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and/or associated documentation files (the
"Materials"), to deal in the Materials without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Materials, and to
permit persons to whom the Materials are furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Materials.

THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.
```

### VSOP87 Earth series (`src/core/astro/vsop87_earth_data.cpp`)

- P. Bretagnon and G. Francou, *Planetary theories in rectangular and spherical variables:
  VSOP87 solutions*, A&A 202, 309 (1988). File `VSOP87D.ear` from IMCCE
  (<ftp://ftp.imcce.fr/pub/ephem/planets/vsop87/>, also CDS VizieR VI/81).
- Truncated to terms with amplitude ≥ 3·10⁻⁸ (385 terms) by `scripts/data/gen_astro_tables.py`.
- IMCCE distributes the series publicly without an explicit licence. The converted,
  truncated table is distributed with this project under the **MIT License**, with credit to
  the authors (project decision, `docs/SPECIFICATION.md` §12.4).

### Lunar series, Meeus tables 47.A and 47.B (`src/core/astro/moon_tables_data.cpp`)

- J. Meeus, *Astronomical Algorithms*, 2nd ed. (Willmann-Bell, 1998), ch. 47 (abridged
  ELP-2000/82 of Chapront-Touzé and Chapront).
- Coefficients transcribed from Sonia Keys' Go package
  <https://github.com/soniakeys/meeus> (`v3/moonposition`), MIT licence:

```
Copyright 2013-2018 Sonia Keys

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### Yale Bright Star Catalogue, 5th Revised Ed. (`assets/sky/bsc5.csv`)

- D. Hoffleit and W. H. Warren Jr., NASA Astronomical Data Center (1991), CDS catalogue V/50.
- Positions, proper motions, V magnitudes and B−V colours of 9096 stars, converted by
  `scripts/data/gen_bsc5.py`. Installed as `/opt/drone_simulator/share/sky/bsc5.csv`.
- NASA ADC and the CDS distribute the catalogue publicly without an explicit licence. The
  converted extract is distributed with this project under the **MIT License**, with credit
  to the authors (project decision, `docs/SPECIFICATION.md` §12.4).

### World Magnetic Model WMM2025 (`src/core/geo/wmm_data.cpp`, `tests/fixtures/geo/wmm_test_values.csv`)

- Chulliat, A., W. Brown, M. Nair, N. Gomez Perez, L.-Y. Young, C. Watson, N. Boneh,
  C. Beggan, B. Meyer and M. Paniccia, 2025. *The US/UK World Magnetic Model for 2025-2030:
  Technical Report*, NOAA National Centers for Environmental Information,
  <https://doi.org/10.25923/prbc-s316>.
- Coefficients (`WMM.COF`) and test values (`WMM2025_TestValues.txt`) from `WMM2025COF.zip`,
  <https://www.ncei.noaa.gov/products/world-magnetic-model>, converted by
  `scripts/data/gen_wmm.py`. The model is evaluated by this project's own code
  (`src/core/geo/magnetic.cpp`), written from the formulas in the technical report.
- **Notice (17 U.S.C. 403):** the WMM2025 coefficients and test values are U.S. Government
  material produced by NOAA NCEI (with the British Geological Survey), are in the public
  domain and are not subject to copyright protection in the United States. NOAA and BGS do
  not endorse this program.

### Moon albedo map (`assets/sky/moon_albedo.pgm`)

- From NASA's Scientific Visualization Studio, *CGI Moon Kit* (<https://svs.gsfc.nasa.gov/4720>),
  file `lroc_color_poles_1k.jpg`, based on the LRO Camera (LROC) Wide Angle Camera mosaic.
  Converted to greyscale by `scripts/data/gen_moon_texture.py`. Installed as
  `/opt/drone_simulator/share/sky/moon_albedo.pgm`.
- Work of the U.S. Government, not subject to copyright in the United States; used under the
  NASA media usage guidelines (`LICENSES/LicenseRef-NASA-Public-Domain.txt`).
  Credit: NASA's Scientific Visualization Studio / LRO / LROC (Arizona State University).
  NASA does not endorse this program.

### B612 Regular font (`assets/fonts/B612-Regular.ttf`)

- HUD text. B612 was designed for aircraft cockpit displays (Airbus, ENAC, Intactile Design).
  Copyright 2012 The B612 Project Authors (<https://github.com/polarsys/b612>). From the Google
  Fonts repository (<https://github.com/google/fonts/tree/main/ofl/b612>), unmodified.
  Installed as `/opt/drone_simulator/share/fonts/B612-Regular.ttf`.
- SIL Open Font License 1.1 (`LICENSES/OFL-1.1.txt`; the upstream copy is
  `assets/fonts/B612-OFL.txt`).

### DSEG14 Classic Regular font (`assets/fonts/DSEG14Classic-Regular.ttf`)

- HUD text, as on a 14-segment display. Copyright (c) 2017 keshikan (<http://www.keshikan.net>),
  with Reserved Font Name "DSEG". DSEG v0.46 (<https://github.com/keshikan/DSEG>), unmodified.
  Installed as `/opt/drone_simulator/share/fonts/DSEG14Classic-Regular.ttf`.
- SIL Open Font License 1.1 (`LICENSES/OFL-1.1.txt`; the upstream copy is
  `assets/fonts/DSEG-LICENSE.txt`).

### OpenGL Mathematics (GLM) 0.9.9.8 — header-only, compiled into the binary

- © 2005 G-Truc Creation — <https://github.com/g-truc/glm>
- Used under the **MIT License** (GLM is dual-licensed: Happy Bunny License or MIT).
- Taken from the Debian package `libglm-dev` at build time; only its templates that the
  program instantiates end up in the binary.

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### JSON for Modern C++ (nlohmann/json) 3.11 — header-only, compiled into the binary

- © 2013-2022 Niels Lohmann — <https://github.com/nlohmann/json>
- Used under the **MIT License**. Reads and writes vehicle profiles, wind levels and input
  mappings. Taken from the Debian package `nlohmann-json3-dev` when installed, otherwise from
  the pinned upstream release 3.11.3 (downloaded by CMake at configure time).

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Methods implemented from publications (no code copied)

- Atmospheric scattering: E. Bruneton, *Precomputed Atmospheric Scattering* (2008, rev. 2017)
  for the transmittance table layout; S. Hillaire, *A Scalable and Production Ready Sky and
  Atmosphere Rendering Technique* (EGSR 2020) for the multiple-scattering and sky-view tables.
- ACES filmic tone-mapping curve fit: K. Narkowicz (2015).
- CIE colour-matching function fit: C. Wyman, P.-P. Sloan, P. Shirley (JCGT 2013).
- Star colour temperature from B−V: F. J. Ballesteros (2012).

## Used as shared libraries from the operating system (not bundled)

### Qt 6 (Core, Gui, Widgets, OpenGL, OpenGLWidgets, Sql + SQLite plugin)

- © The Qt Company Ltd. and other contributors — <https://www.qt.io/>
- Used under the **GNU Lesser General Public License v3** (LGPL-3.0-only).
- Qt is linked dynamically and installed from the Debian packages (`libqt6*`), so it can be
  replaced by the user. Its source code is available from Debian
  (`apt source qt6-base`) and from <https://download.qt.io/>.
- Licence texts are installed by Debian in `/usr/share/doc/libqt6core6t64/copyright`
  (and the corresponding files of the other Qt packages).

### SDL 3 (joystick and gamepad subsystem only)

- © 1997-2025 Sam Lantinga and the SDL contributors — <https://www.libsdl.org/>
- Used under the **zlib License**; linked dynamically and installed from the Debian package
  `libsdl3-0` (licence text in `/usr/share/doc/libsdl3-0/copyright`). Only the joystick and
  gamepad subsystem is initialised (RC transmitters and gamepads, FR-IN-01).

### libcurl (HTTPS to elevation data providers)

- © 1996-2025 Daniel Stenberg and the curl contributors — <https://curl.se/>
- Used under the **curl License** (MIT/X derivative); linked dynamically and installed from the
  Debian package `libcurl4t64` (licence text in `/usr/share/doc/libcurl4t64/copyright`). Used only
  by `core/net` for requests to the data providers the user configures (§4.6).

### GDAL 3.10 (elevation rasters, orthophotos, imagery tiles, COG writing)

- © 1998-2024 Frank Warmerdam, Even Rouault and the GDAL contributors — <https://gdal.org/>
- Used under the **MIT (Expat) licence**; linked dynamically and installed from the Debian
  package `libgdal36` (licence texts in `/usr/share/doc/libgdal36/copyright`). Used only by
  `core/terrain`, with every network driver disabled.

### PROJ 9 (coordinate transformations, through GDAL)

- © the PROJ contributors — <https://proj.org/>
- Used under the **MIT (Expat) licence**; linked dynamically from the Debian package
  `libproj25` (`/usr/share/doc/libproj25/copyright`).

### LASzip 3.5 (LAS / LAZ point clouds)

- © 2007-2022 rapidlasso GmbH (Martin Isenburg) — <https://laszip.org/>
- Used under the **Apache License 2.0**; linked dynamically and installed from the Debian
  package `liblaszip8` (`/usr/share/doc/liblaszip8/copyright`).

### QtKeychain 0.15 (API keys in the system keyring)

- © 2011-2015 Frank Osterfeld and the QtKeychain contributors —
  <https://github.com/frankosterfeld/qtkeychain>
- Used under the **BSD 2-Clause licence**; linked dynamically from the Debian package
  `libqt6keychain1` (`/usr/share/doc/libqt6keychain1/copyright`).

### Open Asset Import Library (Assimp) 5.4 (glTF 2.0 and OBJ model files)

- © 2006-2024 assimp team — <https://github.com/assimp/assimp>
- Used under the **BSD 3-Clause License**; linked dynamically and installed from the Debian
  package `libassimp5` (licence text in `/usr/share/doc/libassimp5/copyright`). Used only by
  `core/graphics` to read the vehicle and item model files the user or a mission names
  (FR-GFX-12).

### OpenGL driver (libGL / libOpenGL / GLVND)

Provided by the system graphics driver; no code is included.

### IANA time zone database (`tzdata`)

Read at run time through the C++ standard library (`std::chrono::tzdb`) for local time and
daylight saving time. Public domain; installed by the Debian `tzdata` package.

## Data fetched at run time (not shipped)

Elevation data, imagery tiles and the data of the built-in real-terrain missions are not part
of the source tree or the package. The application (online providers) and
`share/scripts/fetch_mission_data.py` (mission data) download them from their publishers into
the user's own folders, under the publishers' terms, which are shown before downloading and
in Settings › Data providers. The mission data is listed in `scripts/data/mission_data.json`:

- **swissALTI3D** — © swisstopo; free use, including commercial use and redistribution, with
  the source named (<https://www.swisstopo.admin.ch/en/terms-of-use-free-geodata-and-geoservices>).
- **Copernicus DEM GLO-30** — © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH
  2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved;
  free use and redistribution with this attribution.
- **PNOA orthophotos** — © Instituto Geográfico Nacional de España, CC BY 4.0.

The built-in online providers (`share/providers/`) fetch, while flying:

- **Géoplateforme** (cartes.gouv.fr: RGE ALTI®, LiDAR HD, BD ALTI®, BD ORTHO®, Plan IGN) —
  © IGN, Licence Ouverte / Open Licence 2.0 (Etalab), free use with the source named
  (<https://cartes.gouv.fr/cgu>).
- **OpenStreetMap standard map** — map data © OpenStreetMap contributors, Open Database
  Licence 1.0 (<https://www.openstreetmap.org/copyright>); tiles from the OpenStreetMap
  Foundation's servers under its tile usage policy
  (<https://operations.osmfoundation.org/policies/tiles/>).
- **IGN PNOA**, **swisstopo SWISSIMAGE**, **USGS The National Map** imagery and
  **OpenTopography** elevation, under the terms shown in Settings › Data providers.

## Build-time only (not part of the binary package)

### GoogleTest 1.15.2

- © Google Inc. — BSD-3-Clause — <https://github.com/google/googletest>
- Downloaded by CMake to build the unit and integration tests only.

### Reference data for the tests (`tests/fixtures/`)

Generated by `scripts/fixtures/*.py`; not installed. Values computed with
[PROJ](https://proj.org/) (MIT), [GeographicLib](https://geographiclib.sourceforge.io/) (MIT)
and [astropy](https://www.astropy.org/) (BSD-3-Clause), or obtained from the
[JPL Horizons](https://ssd.jpl.nasa.gov/horizons/) system and the
[USNO Astronomical Applications](https://aa.usno.navy.mil/data/api) API (U.S. Government works).
`online/geoplateforme-rge-alti.tif` is a 48 × 40 px response of the Géoplateforme elevation
service (RGE ALTI® © IGN, Licence Ouverte 2.0, `LICENSES/etalab-2.0.txt`).
