#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
"""Converts the Yale Bright Star Catalogue, 5th Revised Ed. (Hoffleit & Warren 1991, ADC V/50)
into assets/sky/bsc5.csv used by core/astro (FR-AST-08).

Source file: catalog / bsc5.dat (fixed-width, see the V/50 ReadMe), e.g. from
  https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz  or  http://tdc-www.harvard.edu/catalogs/bsc5.dat.gz
Entries removed from the catalogue (no J2000 position) are skipped.

Usage: gen_bsc5.py bsc5.dat.gz assets/sky/bsc5.csv
"""
import gzip
import sys


def field(line, first, last):
    """Bytes first..last (1-based, inclusive) as stripped text."""
    return line[first - 1:last].strip()


def number(text):
    return float(text) if text else None


def main(source, target):
    opener = gzip.open if source.endswith(".gz") else open
    rows = []
    with opener(source, "rt", encoding="latin-1") as f:
        for line in f:
            line = line.rstrip("\n").ljust(197)
            if not field(line, 76, 77):        # Removed entry: no J2000 position.
                continue
            hr = int(field(line, 1, 4))
            name = " ".join(field(line, 5, 14).split())
            ra = (int(field(line, 76, 77)) + int(field(line, 78, 79)) / 60.0
                  + float(field(line, 80, 83)) / 3600.0) * 15.0
            sign = -1.0 if field(line, 84, 84) == "-" else 1.0
            dec = sign * (int(field(line, 85, 86)) + int(field(line, 87, 88)) / 60.0
                          + int(field(line, 89, 90)) / 3600.0)
            vmag = number(field(line, 103, 107))
            bv = number(field(line, 110, 114))
            pm_ra = number(field(line, 149, 154)) or 0.0   # mu_alpha * cos(delta), "/yr
            pm_dec = number(field(line, 155, 160)) or 0.0
            if vmag is None:
                continue
            rows.append((hr, name, ra, dec, pm_ra, pm_dec, vmag, bv))
    with open(target, "w", newline="\n") as out:
        out.write("# Yale Bright Star Catalogue, 5th Revised Ed. (Hoffleit & Warren 1991), "
                  "converted by scripts/data/gen_bsc5.py.\n")
        out.write("# Positions: FK5, equinox and epoch J2000. pm_ra is mu_alpha*cos(delta).\n")
        out.write("hr,name,ra_deg,dec_deg,pm_ra_arcsec_yr,pm_dec_arcsec_yr,vmag,b_v\n")
        for hr, name, ra, dec, pm_ra, pm_dec, vmag, bv in rows:
            bv_text = "" if bv is None else f"{bv:.2f}"
            out.write(f"{hr},{name.replace(',', ' ')},{ra:.6f},{dec:.6f},{pm_ra:.3f},"
                      f"{pm_dec:.3f},{vmag:.2f},{bv_text}\n")
    print(f"wrote {len(rows)} stars to {target}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
