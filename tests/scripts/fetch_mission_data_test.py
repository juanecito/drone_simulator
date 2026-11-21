#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
"""Tests of scripts/data/fetch_mission_data.py against a local HTTP server (never the real
publishers): downloads with size and SHA-256 checks, files already present, world files,
attribution notices, mission selection, and the refusals (a changed file, an error page
instead of an image, plain HTTP, paths leaving the library)."""
import contextlib
import functools
import hashlib
import http.server
import importlib.util
import io
import json
import pathlib
import sys
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "data" / "fetch_mission_data.py"

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
spec = importlib.util.spec_from_file_location("fetch_mission_data", SCRIPT)
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)

JPEG = b"\xff\xd8\xff\xe0" + b"jpeg" * 100


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


class FetchMissionDataTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = pathlib.Path(self.tmp.name)
        self.served = base / "served"
        self.library = base / "library"
        self.served.mkdir()
        (self.served / "dem.tif").write_bytes(b"II*\x00" + b"x" * 1000)
        (self.served / "photo.jpg").write_bytes(JPEG)
        (self.served / "error.jpg").write_bytes(b"<ServiceExceptionReport/>")
        (self.served / "error.xml").write_bytes(b"<ServiceExceptionReport>no</ServiceExceptionReport>")
        handler = functools.partial(Quiet, directory=str(self.served))
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.manifest = base / "manifest.json"
        self.write_manifest()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.tmp.cleanup()

    def dem_entry(self, **changes):
        body = (self.served / "dem.tif").read_bytes()
        entry = {"path": "site/dem.tif", "url": f"{self.url}/dem.tif", "bytes": len(body),
                 "sha256": hashlib.sha256(body).hexdigest()}
        entry.update(changes)
        return entry

    def write_manifest(self, dem=None, photo=None):
        datasets = [
            {"id": "dem", "name": "Test DEM", "missions": ["m-dem"],
             "attribution": "© Test agency", "licence": "CC BY 4.0", "termsUrl": "https://x.org",
             "files": [dem or self.dem_entry()]},
            {"id": "photo", "name": "Test photo", "missions": ["m-photo"],
             "attribution": "© Photo agency", "licence": "CC BY 4.0", "termsUrl": "https://x.org",
             "files": [photo or {"path": "site/photo.jpg", "url": f"{self.url}/photo.jpg",
                                 "contentType": "image/jpeg",
                                 "worldFile": {"path": "site/photo.jgw", "west": 448000,
                                               "north": 4648700, "pixelSizeM": 2.0}}]},
        ]
        self.manifest.write_text(json.dumps({"manifestVersion": 1, "datasets": datasets}))

    def run_script(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = fetch.main(["--manifest", str(self.manifest), "--library", str(self.library),
                               "--allow-http", "--yes", "--timeout", "10", *args])
        return code, out.getvalue(), err.getvalue()

    def test_fetches_checks_and_describes_the_data(self):
        code, out, err = self.run_script()
        self.assertEqual(code, 0, err)
        self.assertEqual((self.library / "site" / "dem.tif").read_bytes(),
                         (self.served / "dem.tif").read_bytes())
        self.assertEqual((self.library / "site" / "photo.jpg").read_bytes(), JPEG)
        # World file: pixel size, rotations, centre of the top-left pixel.
        world = (self.library / "site" / "photo.jgw").read_text().split()
        self.assertEqual([float(v) for v in world], [2.0, 0.0, 0.0, -2.0, 448001.0, 4648699.0])
        notice = (self.library / "site" / "ATTRIBUTION.txt").read_text()
        self.assertIn("© Test agency", notice)
        self.assertIn("© Photo agency", notice)
        self.assertIn("CC BY 4.0", out)
        self.assertFalse(list(self.library.rglob("*.part")))

        # Again: nothing to fetch; a lost world file is written again.
        (self.library / "site" / "photo.jgw").unlink()
        code, out, _ = self.run_script()
        self.assertEqual(code, 0)
        self.assertIn("All mission data is in", out)
        self.assertTrue((self.library / "site" / "photo.jgw").exists())

    def test_list_dry_run_and_mission_selection(self):
        code, out, _ = self.run_script("--list")
        self.assertEqual(code, 0)
        self.assertIn("0 of 1 file(s) present", out)
        code, out, _ = self.run_script("--dry-run", "--mission", "m-photo")
        self.assertEqual(code, 0)
        self.assertIn("photo.jpg", out)
        self.assertNotIn("dem.tif", out)
        self.assertFalse(self.library.exists() and any(self.library.rglob("*.jpg")))
        code, _, err = self.run_script("--mission", "nope")
        self.assertEqual(code, 2)
        self.assertIn("no data for mission(s): nope", err)
        self.run_script("--mission", "m-dem")
        self.assertTrue((self.library / "site" / "dem.tif").exists())
        self.assertFalse((self.library / "site" / "photo.jpg").exists())

    def test_a_changed_file_is_refused_and_not_kept(self):
        self.write_manifest(dem=self.dem_entry(sha256="0" * 64))
        code, _, err = self.run_script()
        self.assertEqual(code, 1)
        self.assertIn("differs from the expected", err)
        self.assertFalse((self.library / "site" / "dem.tif").exists())
        self.assertFalse(list(self.library.rglob("*.part")))

    def test_an_error_page_is_not_an_image(self):
        for name, expected in (("error.xml", "not image/jpeg"), ("error.jpg", "not an image")):
            self.write_manifest(photo={"path": "site/photo.jpg", "url": f"{self.url}/{name}",
                                       "contentType": "image/jpeg"})
            code, _, err = self.run_script("--mission", "m-photo")
            self.assertEqual(code, 1, name)
            self.assertIn(expected, err)
            self.assertFalse((self.library / "site" / "photo.jpg").exists())

    def test_plain_http_and_paths_outside_the_library_are_refused(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = fetch.main(["--manifest", str(self.manifest), "--library", str(self.library),
                               "--yes", "--mission", "m-dem"])
        self.assertEqual(code, 1)
        self.assertIn("only https:// URLs", err.getvalue())
        for bad in ("../evil.tif", "/etc/evil.tif", "a\\b.tif"):
            self.write_manifest(dem=self.dem_entry(path=bad))
            code, _, err = self.run_script()
            self.assertEqual(code, 2, bad)
            self.assertIn("not a path inside the library", err)

    def test_the_shipped_manifest_is_valid(self):
        manifest = fetch.load_manifest(ROOT / "scripts" / "data" / "mission_data.json")
        for dataset in manifest["datasets"]:
            for entry in dataset["files"]:
                self.assertTrue(entry["url"].startswith("https://"), entry["url"])


if __name__ == "__main__":
    unittest.main()
