#!/usr/bin/env python3
"""Reorganize a flat tiles/ directory into the sharded layout.

Flat:     tiles/tile_<tx>_<ty>.bin
Sharded:  tiles/<tx>/tile_<tx>_<ty>.bin

Moves each tile into a subdirectory named after its tx column so every FAT
directory on the SD card stays small and fopen() on the ESP is fast. The binary
content of each tile is untouched.

Usage:
    python reshard_tiles.py <tiles_dir> [--copy] [--dry-run]

    <tiles_dir>   directory containing the flat tile_*_*.bin files
                  (e.g. ./tiles  or  /Volumes/SDCARD/tiles)
    --copy        copy instead of move (leaves the originals in place)
    --dry-run     print what would happen without touching the filesystem
"""
import argparse
import os
import re
import shutil
import sys

TILE_RE = re.compile(r"^tile_(-?\d+)_(-?\d+)\.bin$")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tiles_dir", help="directory with flat tile_*_*.bin files")
    ap.add_argument("--copy", action="store_true", help="copy instead of move")
    ap.add_argument("--dry-run", action="store_true", help="only print actions")
    args = ap.parse_args()

    tiles_dir = os.path.abspath(args.tiles_dir)
    if not os.path.isdir(tiles_dir):
        sys.exit(f"error: not a directory: {tiles_dir}")

    moved = 0
    skipped = 0
    for entry in os.listdir(tiles_dir):
        src = os.path.join(tiles_dir, entry)
        if not os.path.isfile(src):
            continue
        m = TILE_RE.match(entry)
        if not m:
            skipped += 1
            continue

        tx = m.group(1)
        subdir = os.path.join(tiles_dir, tx)
        dst = os.path.join(subdir, entry)

        if args.dry_run:
            print(f"{'copy' if args.copy else 'move'}: {entry} -> {tx}/")
            moved += 1
            continue

        os.makedirs(subdir, exist_ok=True)
        if args.copy:
            shutil.copy2(src, dst)
        else:
            shutil.move(src, dst)
        moved += 1

    action = "would move" if args.dry_run else ("copied" if args.copy else "moved")
    print(f"\n{action} {moved} tiles into per-tx subdirectories "
          f"({skipped} non-tile entries skipped).")


if __name__ == "__main__":
    main()
