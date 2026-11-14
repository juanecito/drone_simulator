#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
"""Converts NASA's CGI Moon Kit colour map into the moon albedo texture.

Source: NASA's Scientific Visualization Studio, "CGI Moon Kit" (https://svs.gsfc.nasa.gov/4720),
file lroc_color_poles_1k.jpg (1024 x 512, equirectangular, centred on 0 deg longitude),
adapted from the LROC WAC Hapke-normalised mosaic. Public domain (U.S. Government work).

Output: binary PGM (P5), 8-bit greyscale, sRGB-encoded Rec.709 luma, 1024 x 512.

Usage: gen_moon_texture.py lroc_color_poles_1k.jpg assets/sky/moon_albedo.pgm
Needs Pillow.
"""
import sys

from PIL import Image


def main(source: str, target: str) -> None:
    image = Image.open(source).convert("RGB")
    if image.size != (1024, 512):
        image = image.resize((1024, 512), Image.LANCZOS)
    linear = []
    for r, g, b in image.getdata():
        def lin(c: int) -> float:
            c /= 255.0
            return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
        y = 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
        s = y * 12.92 if y <= 0.0031308 else 1.055 * y ** (1 / 2.4) - 0.055
        linear.append(max(0, min(255, round(s * 255))))
    with open(target, "wb") as out:
        out.write(b"P5\n# NASA SVS CGI Moon Kit (LROC), public domain; scripts/data/gen_moon_texture.py\n")
        out.write(b"1024 512\n255\n")
        out.write(bytes(linear))
    print(f"wrote {target}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
