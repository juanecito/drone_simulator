#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
"""Fetches the terrain data of the built-in real-terrain missions into the geodata library.

The data (elevation rasters and orthophotos) is published by national mapping
agencies and the Copernicus programme under licences that allow redistribution with
attribution, but it is large, so the simulator does not ship it. This script downloads it once
from the publishers into ~/.drone_simulation/library/ (the missions refer to it as
"library:<path>"); after that the missions are available, also offline.

The list of files is mission_data.json next to this script. Files with a known SHA-256 are
checked; a file already present with the right size and hash is not downloaded again. Files
are written through a ".part" file and renamed when complete.

Usage:
  fetch_mission_data.py [--library DIR] [--mission ID ...] [--list] [--dry-run] [--yes]

Only the Python standard library is needed.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
import urllib.error
import urllib.request

USER_AGENT = "drone-simulator-mission-data/1"
CHUNK = 1 << 20
IMAGE_SIGNATURES = (b"\xff\xd8\xff", b"\x89PNG\r\n\x1a\n")


class FetchError(Exception):
    """A file could not be fetched or checked."""


def default_library():
    return pathlib.Path.home() / ".drone_simulation" / "library"


def load_manifest(path):
    with open(path, encoding="utf-8") as f:
        manifest = json.load(f)
    if manifest.get("manifestVersion") != 1:
        raise FetchError(f"{path}: unsupported manifestVersion")
    for dataset in manifest["datasets"]:
        for key in ("id", "name", "missions", "attribution", "licence", "termsUrl", "files"):
            if key not in dataset:
                raise FetchError(f"{path}: dataset {dataset.get('id', '?')} has no {key}")
        for entry in dataset["files"]:
            check_relative(entry["path"])
            if "worldFile" in entry:
                check_relative(entry["worldFile"]["path"])
    return manifest


def check_relative(path):
    """Rejects paths that would leave the library (the missions' path rules)."""
    p = pathlib.PurePosixPath(path)
    if p.is_absolute() or ".." in p.parts or "\\" in path or not p.parts:
        raise FetchError(f"{path}: not a path inside the library")


def select(manifest, missions):
    if not missions:
        return manifest["datasets"]
    known = {m for d in manifest["datasets"] for m in d["missions"]}
    unknown = sorted(set(missions) - known)
    if unknown:
        raise FetchError("no data for mission(s): " + ", ".join(unknown) +
                         " (known: " + ", ".join(sorted(known)) + ")")
    return [d for d in manifest["datasets"] if set(d["missions"]) & set(missions)]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


def is_present(target, entry):
    """True when @p target already holds the file of @p entry."""
    if not target.is_file():
        return False
    if "bytes" in entry and target.stat().st_size != entry["bytes"]:
        return False
    if "sha256" in entry:
        return sha256_of(target) == entry["sha256"]
    return target.stat().st_size > 0


def size_text(n):
    for unit in ("bytes", "kB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "bytes" else f"{n:.1f} {unit}"
        n /= 1000.0
    return ""


def download(url, target, entry, allow_http, timeout, progress):
    if not url.startswith("https://") and not (allow_http and url.startswith("http://")):
        raise FetchError(f"{url}: only https:// URLs are fetched")
    part = target.with_name(target.name + ".part")
    target.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    h = hashlib.sha256()
    size = 0
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response, open(part, "wb") as out:
            content_type = response.headers.get("Content-Type", "")
            expected = entry.get("contentType")
            if expected and not content_type.startswith(expected):
                body = response.read(300).decode("utf-8", "replace").strip()
                raise FetchError(f"{url}: got {content_type or 'no content type'}, "
                                 f"not {expected}: {body[:200]}")
            total = entry.get("bytes") or int(response.headers.get("Content-Length") or 0)
            first = b""
            for block in iter(lambda: response.read(CHUNK), b""):
                if not first:
                    first = block[:16]
                h.update(block)
                out.write(block)
                size += len(block)
                progress(size, total)
        if expected and expected.startswith("image/") and not first.startswith(IMAGE_SIGNATURES):
            raise FetchError(f"{url}: the answer is not an image")
        if "bytes" in entry and size != entry["bytes"]:
            raise FetchError(f"{url}: {size} bytes, expected {entry['bytes']}")
        if "sha256" in entry and h.hexdigest() != entry["sha256"]:
            raise FetchError(f"{url}: SHA-256 {h.hexdigest()} differs from the expected "
                             f"{entry['sha256']} (has the publisher changed the file?)")
        os.replace(part, target)
    except (urllib.error.URLError, OSError) as e:
        raise FetchError(f"{url}: {e}") from e
    finally:
        if part.exists():
            part.unlink()


def write_world_file(library, world):
    """ESRI world file of a north-up image: pixel size, rotation, west and north edges given
    as the centre of the top-left pixel."""
    s = float(world["pixelSizeM"])
    text = "\n".join(f"{v:.6f}" for v in (s, 0.0, 0.0, -s, world["west"] + s / 2,
                                           world["north"] - s / 2)) + "\n"
    path = library / world["path"]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="ascii")


def write_notice(library, dataset):
    """ATTRIBUTION.txt next to the files: where they come from and on which terms."""
    folders = {pathlib.PurePosixPath(e["path"]).parts[0] for e in dataset["files"]}
    for folder in folders:
        path = library / folder / "ATTRIBUTION.txt"
        lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
        header = f"{dataset['name']}"
        if header in lines:
            continue
        lines += [header, f"  Attribution: {dataset['attribution']}",
                  f"  Licence: {dataset['licence']}", f"  Terms: {dataset['termsUrl']}", ""]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv=None):
    here = pathlib.Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--library", type=pathlib.Path, default=default_library(),
                        help="geodata library folder (default: %(default)s)")
    parser.add_argument("--mission", action="append", default=[], metavar="ID",
                        help="only the data of this mission (repeatable)")
    parser.add_argument("--manifest", type=pathlib.Path, default=here / "mission_data.json",
                        help=argparse.SUPPRESS)
    parser.add_argument("--allow-http", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--list", action="store_true", help="list the data and its state")
    parser.add_argument("--dry-run", action="store_true", help="show what would be fetched")
    parser.add_argument("--yes", "-y", action="store_true", help="do not ask before fetching")
    parser.add_argument("--timeout", type=float, default=60.0, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    try:
        manifest = load_manifest(args.manifest)
        datasets = select(manifest, args.mission)
    except (FetchError, OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    library = args.library.expanduser()

    missing = []
    for d in datasets:
        state = []
        for entry in d["files"]:
            present = is_present(library / entry["path"], entry)
            state.append(present)
            if not present:
                missing.append((d, entry))
            elif "worldFile" in entry and not (library / entry["worldFile"]["path"]).is_file():
                if not (args.list or args.dry_run):
                    write_world_file(library, entry["worldFile"])
        if args.list:
            have = sum(state)
            print(f"{d['id']}: {d['name']}\n  missions: {', '.join(d['missions'])}\n"
                  f"  {have} of {len(state)} file(s) present\n  {d['licence']}\n"
                  f"  {d['termsUrl']}")
    if args.list:
        return 0
    if not missing:
        print(f"All mission data is in {library}.")
        for d in datasets:
            write_notice(library, d)
        return 0

    known = sum(e.get("bytes", 0) for _, e in missing)
    unknown = sum(1 for _, e in missing if "bytes" not in e)
    print(f"Mission data to fetch into {library}:")
    for d in {d["id"]: d for d, _ in missing}.values():
        files = [e for dd, e in missing if dd is d]
        print(f"- {d['name']}: {len(files)} file(s)\n  {d['attribution']}\n"
              f"  Licence: {d['licence']}\n  Terms: {d['termsUrl']}")
    total = size_text(known) + (f" plus {unknown} file(s) of unknown size" if unknown else "")
    print(f"Total: {total}.")
    if args.dry_run:
        for _, e in missing:
            print(f"  {e['url']}\n    → {library / e['path']}")
        return 0
    if not args.yes:
        if not sys.stdin.isatty():
            print("Run with --yes to fetch without asking.", file=sys.stderr)
            return 2
        if input("Fetch it now? [y/N] ").strip().lower() not in ("y", "yes"):
            return 1

    failures = 0
    for i, (d, entry) in enumerate(missing, 1):
        target = library / entry["path"]
        label = f"[{i}/{len(missing)}] {entry['path']}"

        def progress(done, total, label=label):
            if sys.stdout.isatty():
                pct = f" {100 * done // total}%" if total else ""
                print(f"\r{label}: {size_text(done)}{pct}   ", end="", flush=True)

        try:
            download(entry["url"], target, entry, args.allow_http, args.timeout, progress)
            if "worldFile" in entry:
                write_world_file(library, entry["worldFile"])
            write_notice(library, d)
            print(f"\r{label}: done ({size_text(target.stat().st_size)})      ")
        except FetchError as e:
            failures += 1
            print(f"\r{label}: failed\n  {e}", file=sys.stderr)
    if failures:
        print(f"{failures} file(s) failed; run the script again to retry.", file=sys.stderr)
        return 1
    print("Done. Press Reload in the Missions tab (or restart the simulator) to play them.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
