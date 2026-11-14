#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
"""Generates reference tables for the core/astro unit tests (SPECIFICATION §8.1, NFR-REAL-10).

Sources (all public, results are US Government works or computed locally):
  * JPL Horizons API      — apparent topocentric Sun/Moon azimuth & elevation (airless and
                            refracted), Moon illuminated fraction and sub-solar position angle.
  * USNO Astronomical Applications API — rise/set/transit/civil twilight, lunar phases.
  * astropy (ERFA, IAU 2006/2000A) — apparent places of catalogue stars.
Tests never contact these services; run this script only to refresh tests/fixtures/astro.

Usage: python3 -m venv .venv && .venv/bin/pip install astropy
       .venv/bin/python scripts/fixtures/gen_astro_fixtures.py tests/fixtures/astro assets/sky/bsc5.csv
"""
import csv
import json
import random
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

HORIZONS = "https://ssd.jpl.nasa.gov/api/horizons.api"
USNO = "https://aa.usno.navy.mil/api"
USER_AGENT = "drone-simulator-fixture-generator"

# name, latitude, longitude, height (m)
SITES = [
    ("madrid", 40.4168, -3.7038, 667.0),
    ("sydney", -33.8688, 151.2093, 58.0),
    ("quito", -0.1807, -78.4678, 2850.0),
    ("tromso", 69.6492, 18.9553, 10.0),
    ("ushuaia", -54.8019, -68.3030, 20.0),
    ("honolulu", 21.3069, -157.8583, 5.0),
]
JD_1950 = 2433282.5
JD_2100 = 2488069.5


def get(url, params):
    query = urllib.parse.urlencode(params, safe="',@")
    request = urllib.request.Request(f"{url}?{query}", headers={"User-Agent": USER_AGENT})
    for attempt in range(5):
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                return response.read().decode()
        except Exception as error:  # noqa: BLE001 - retry any network error
            print(f"  retry {attempt + 1}: {error}")
            time.sleep(3 * (attempt + 1))
    raise RuntimeError(f"request failed: {url}")


def horizons(body, site, jds, apparent):
    _, lat, lon, height = site
    text = get(HORIZONS, {
        "format": "text", "COMMAND": f"'{body}'", "EPHEM_TYPE": "'OBSERVER'",
        "CENTER": "'coord@399'", "COORD_TYPE": "'GEODETIC'",
        "SITE_COORD": f"'{lon},{lat},{height / 1000.0}'",
        "TLIST": "'" + " ".join(f"{jd:.6f}" for jd in jds) + "'",
        "TLIST_TYPE": "'JD'", "TIME_TYPE": "'UT'", "CAL_FORMAT": "'JD'",
        "QUANTITIES": "'4,10,16,20'", "ANG_FORMAT": "'DEG'", "APPARENT": f"'{apparent}'",
        "CSV_FORMAT": "'YES'", "EXTRA_PREC": "'YES'",
    })
    lines = text.splitlines()
    start = lines.index("$$SOE")
    end = lines.index("$$EOE")
    header = [h.strip() for h in lines[start - 2].split(",")]
    rows = []
    for line in lines[start + 1:end]:
        values = [v.strip() for v in line.split(",")]
        rows.append(dict(zip(header, values)))
    return rows


def column(row, prefix):
    for key, value in row.items():
        if key.startswith(prefix):
            return value
    raise KeyError(prefix)


def gen_horizons(out, rng):
    sun_rows, moon_rows = [], []
    for site in SITES:
        jds = sorted(rng.uniform(JD_1950, JD_2100) for _ in range(40))
        for body, rows in (("10", sun_rows), ("301", moon_rows)):
            print(f"Horizons {body} {site[0]}")
            airless = horizons(body, site, jds, "AIRLESS")
            refracted = horizons(body, site, jds, "REFRACTED")
            for jd, a, r in zip(jds, airless, refracted):
                row = [site[0], site[1], site[2], site[3], f"{jd:.6f}",
                       column(a, "Azi"), column(a, "Elev"), column(r, "Elev"),
                       column(a, "delta")]
                if body == "301":
                    row += [column(a, "Illu%"), column(a, "SN.ang")]
                rows.append(row)
            time.sleep(1)
    common = ["site", "lat_deg", "lon_deg", "height_m", "jd_ut", "azimuth_deg",
              "elevation_airless_deg", "elevation_refracted_deg", "distance_au"]
    write(out / "horizons_sun.csv", common, sun_rows)
    write(out / "horizons_moon.csv", common + ["illumination_percent", "sub_solar_pa_deg"],
          moon_rows)


def gen_usno(out, rng):
    events = {"Rise": "rise", "Set": "set", "Upper Transit": "transit",
              "Begin Civil Twilight": "civil_dawn", "End Civil Twilight": "civil_dusk"}
    rows = []
    for site in SITES:
        dates = ["2026-06-21", "2026-12-21", "2026-03-20", "1955-02-14", "2099-09-30"]
        dates += [f"{rng.randint(1950, 2099)}-{rng.randint(1, 12):02d}-{rng.randint(1, 28):02d}"
                  for _ in range(5)]
        for date in dates:
            print(f"USNO {site[0]} {date}")
            data = json.loads(get(f"{USNO}/rstt/oneday", {
                "date": date, "coords": f"{site[1]},{site[2]}", "tz": "0"}))["properties"]["data"]
            for body, key in (("sun", "sundata"), ("moon", "moondata")):
                found = {events[e["phen"]]: e["time"] for e in data.get(key, [])
                         if e["phen"] in events}
                wanted = ["rise", "set", "transit"] + (["civil_dawn", "civil_dusk"]
                                                       if body == "sun" else [])
                for event in wanted:
                    rows.append([site[0], site[1], site[2], date, body, event,
                                 found.get(event, "")])
            time.sleep(0.5)
    write(out / "usno_rise_set.csv",
          ["site", "lat_deg", "lon_deg", "date", "body", "event", "time_utc"], rows)

    phases = []
    for start in ("1960-01-01", "2026-01-01", "2095-01-01"):
        print(f"USNO phases {start}")
        data = json.loads(get(f"{USNO}/moon/phases/date", {"date": start, "nump": "16"}))
        for p in data["phasedata"]:
            phases.append([p["phase"], f"{p['year']:04d}-{p['month']:02d}-{p['day']:02d}T"
                           f"{p['time']}:00Z"])
        time.sleep(0.5)
    write(out / "usno_phases.csv", ["phase", "time_utc"], phases)


def gen_stars(out, catalogue):
    import astropy.units as u
    from astropy.coordinates import TETE, SkyCoord
    from astropy.time import Time
    from astropy.utils import iers
    iers.conf.auto_download = False
    wanted = {424, 2491, 7001, 5340, 2326, 4730, 7228, 1708, 5056, 8728}
    stars = []
    with open(catalogue) as f:
        for row in csv.reader(line for line in f if not line.startswith("#")):
            if row[0].isdigit() and int(row[0]) in wanted:
                stars.append(row)
    rows = []
    for when in ("1950-06-01T00:00:00", "2026-11-13T20:00:00", "2100-01-01T12:00:00"):
        t = Time(when, scale="utc")
        for hr, name, ra, dec, pm_ra, pm_dec, *_ in stars:
            c = SkyCoord(ra=float(ra) * u.deg, dec=float(dec) * u.deg, distance=1000 * u.pc,
                         pm_ra_cosdec=float(pm_ra) * 1000 * u.mas / u.yr,
                         pm_dec=float(pm_dec) * 1000 * u.mas / u.yr,
                         radial_velocity=0 * u.km / u.s, frame="fk5", equinox="J2000",
                         obstime=Time("J2000"))
            moved = c.apply_space_motion(new_obstime=t)
            apparent = moved.transform_to(TETE(obstime=t))
            rows.append([hr, when + "Z", f"{apparent.ra.deg:.8f}", f"{apparent.dec.deg:.8f}"])
    write(out / "stars_apparent.csv", ["hr", "time_utc", "ra_deg", "dec_deg"], rows)


def write(path, columns, rows):
    with open(path, "w", newline="") as f:
        f.write("# Generated by scripts/fixtures/gen_astro_fixtures.py. Do not edit.\n")
        writer = csv.writer(f, lineterminator="\n")
        writer.writerow(columns)
        writer.writerows(rows)
    print(f"wrote {path} ({len(rows)} rows)")


def main(out_dir, catalogue):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261113)
    gen_stars(out, catalogue)
    gen_usno(out, rng)
    gen_horizons(out, rng)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
