# usage: show_tile_streets.py [-h] [--show-coordinates] lat lon

# This script reads OpenStreetMap extracted tile files and prints street names and speed limits.

# positional arguments:
#   lat                   Latitude of the point
#   lon                   Longitude of the point

# optional arguments:
#   -h, --help            show this help message and exit
#   --show-coordinates    Print the coordinates of the road segments (default: False)
#   --scan-all            Scan all tile files for speed limit and street name
#   --plot-tile           Plot all streets in a tile file for speed limit and street name
#       --color-by-speed       Apply color on streets depending on allowed speed
#       --print-all-info       Show all the data within tile file


import struct
import os
from math import floor
import argparse
import matplotlib.pyplot as plt

# -----------------------------
# CONFIGURATION
# -----------------------------
TILE_SIZE = 0.003
OUT_DIR = "tiles/"

# -------- Compute tile index --------
TILE_SIZE_E7 = int(round(TILE_SIZE * 1e7))

def resolution_digits(tile_size_e7):
    d = 0
    while tile_size_e7 % 10 == 0:
        tile_size_e7 //= 10
        d += 1
    return d

TRIM_ZEROS = resolution_digits(TILE_SIZE_E7)
FILENAME_SCALE = 10 ** TRIM_ZEROS

def filename_coord(value_e7):
    return value_e7 // FILENAME_SCALE

def tile_origin(lat, lon):
    lat_e7 = int(lat * 1e7)
    lon_e7 = int(lon * 1e7)

    origin_lat_e7 = (lat_e7 // TILE_SIZE_E7) * TILE_SIZE_E7
    origin_lon_e7 = (lon_e7 // TILE_SIZE_E7) * TILE_SIZE_E7

    return origin_lat_e7, origin_lon_e7

def tile_id(lat, lon):
    return tile_origin(lat, lon)

# -------- Draw polyline --------
def draw_polyline(coords, street_name):
    lats, lons = zip(*coords)
    plt.figure(figsize=(6, 6))
    plt.plot(lons, lats, marker='o', linestyle='-', color='blue')
    plt.title(f"Street: {street_name}")
    plt.xlabel("Longitude")
    plt.ylabel("Latitude")
    plt.grid(True)
    plt.show(block=True)
    plt.close()

# -------- Read tile --------
def read_tile(lat, lon, show_coordinates):
    tx, ty = tile_id(lat, lon)
    tx_name = filename_coord(tx)
    ty_name = filename_coord(ty)
    filename = f"{OUT_DIR}/tile_{tx_name}_{ty_name}.bin"

    if not os.path.exists(filename):
        print(f"No data for tile ({tx_name}, {ty_name})")
        return

    with open(filename, "rb") as f:
        num_segments = struct.unpack("<I", f.read(4))[0]
        print(f"Tile ({tx_name}, {ty_name}) contains {num_segments} segments:")

        seen = set()   # Unique dataset tracking

        for _ in range(num_segments):

            # ---- Read coordinates ----
            num_points = struct.unpack("<H", f.read(2))[0]
            coords = []
            for _ in range(num_points):
                lat_i, lon_i = struct.unpack("<ii", f.read(8))
                coords.append((lat_i / 1e7, lon_i / 1e7))

            # ---- Read speed limit ----
            speed = struct.unpack("<H", f.read(2))[0]

            # ---- Read name ----
            name_len = struct.unpack("<H", f.read(2))[0]
            name_bytes = f.read(name_len)
            name = name_bytes.decode("utf-8").strip()

            # ---- FILTER: valid name + valid speed ----
            if not name or speed == 0:
                continue

            key = (name, speed)

            # Skip duplicates
            if key in seen:
                continue
            seen.add(key)

            # ---- Output ----
            print(f"\nStreet: {name}, Speed: {speed} km/h")
            if show_coordinates:
                for a, b in coords:
                    print(f"    {a}, {b}")

            # ---- Draw polyline ----
            while True:
                draw = input("Draw this street polyline? (y/n): ").lower().strip()
                if draw in ("y", "n"):
                    break

            if draw == "y":
                draw_polyline(coords, name)

# -------- Scan all tiles --------
def scan_all_tiles():
    if not os.path.exists(OUT_DIR):
        print(f"Tile directory '{OUT_DIR}' not found.")
        return

    files = [f for f in os.listdir(OUT_DIR) if f.endswith(".bin")]
    if not files:
        print("No tile files found.")
        return

    seen = set()
    match_index = 0

    for filename in files:
        path = os.path.join(OUT_DIR, filename)
        with open(path, "rb") as f:
            num_segments = struct.unpack("<I", f.read(4))[0]

            for _ in range(num_segments):

                num_points = struct.unpack("<H", f.read(2))[0]
                coords = []
                for _ in range(num_points):
                    lat_i, lon_i = struct.unpack("<ii", f.read(8))
                    coords.append((lat_i / 1e7, lon_i / 1e7))

                speed = struct.unpack("<H", f.read(2))[0]

                name_len = struct.unpack("<H", f.read(2))[0]
                name_bytes = f.read(name_len)
                name = name_bytes.decode("utf-8").strip()

                # ---- FILTER ONLY VALID DATA ----
                if not name or speed == 0:
                    continue

                key = (name, speed)
                if key in seen:
                    continue
                seen.add(key)

                # ---- Print result ----
                print(f"\nMatch #{match_index} in {filename}:")
                print(f"  Street: {name}")
                print(f"  Speed limit: {speed} km/h")
                print(f"  Coordinates ({len(coords)} points):")
                for a, b in coords:
                    print(f"    {a}, {b}")
                match_index += 1

                # ---- Draw? ----
                while True:
                    draw = input("Draw this street polyline? (y/n): ").lower().strip()
                    if draw in ("y", "n"):
                        break

                if draw == "y":
                    draw_polyline(coords, name)

                # ---- Continue scanning? ----
                while True:
                    cont = input("Continue scanning? (y/n): ").lower().strip()
                    if cont in ("y", "n"):
                        break

                if cont == "n":
                    print("Stopping scan.")
                    return

    print("\nFinished scanning all tiles.")

# -------- Plot all streets in tile --------
def plot_tile_file(tile_path, color_by_speed=False, print_all_info=False):
    if not os.path.exists(tile_path):
        print(f"Tile file '{tile_path}' not found.")
        return

    plt.figure(figsize=(9, 9))

    with open(tile_path, "rb") as f:
        num_segments = struct.unpack("<I", f.read(4))[0]

        if print_all_info:
            print(f"\nTile: {tile_path}")
            print(f"Segments: {num_segments}")

        for idx in range(num_segments):
            # ---- Read geometry ----
            num_points = struct.unpack("<H", f.read(2))[0]
            coords = []
            for _ in range(num_points):
                lat_i, lon_i = struct.unpack("<ii", f.read(8))
                coords.append((lat_i / 1e7, lon_i / 1e7))

            speed = struct.unpack("<H", f.read(2))[0]

            name_len = struct.unpack("<H", f.read(2))[0]
            name = f.read(name_len).decode("utf-8").strip()

            # ---- Filter invalid ----
            if not coords or speed == 0 or not name:
                continue

            lats, lons = zip(*coords)

            # ---- Color selection ----
            if color_by_speed:
                if speed <= 30:
                    color = "green"
                elif speed <= 50:
                    color = "orange"
                else:
                    color = "red"
            else:
                color = "black"

            # ---- Plot line ----
            plt.plot(
                lons,
                lats,
                linewidth=1.2,
                alpha=0.85,
                color=color
            )

            # ---- Print info ----
            if print_all_info:
                print(f"\nSegment #{idx}")
                print(f"  Street: {name}")
                print(f"  Speed limit: {speed} km/h")
                print(f"  Points: {len(coords)}")
                for a, b in coords:
                    print(f"    {a}, {b}")

            # ---- Annotate midpoint ----
            if print_all_info:
                mid = len(coords) // 2
                label = f"{name} ({speed} km/h)"
                plt.text(
                    lons[mid],
                    lats[mid],
                    label,
                    fontsize=7,
                    alpha=0.75,
                    ha="center",
                    va="center"
                )

    plt.title(f"Tile Map View: {os.path.basename(tile_path)}")
    plt.xlabel("Longitude")
    plt.ylabel("Latitude")
    plt.axis("equal")
    plt.grid(True)
    plt.tight_layout()
    plt.show()

# ---------- MAIN ----------
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Read OpenStreetMap tile files and print street names and speed limits."
    )

    parser.add_argument("lat", nargs="?", type=float, help="Latitude")
    parser.add_argument("lon", nargs="?", type=float, help="Longitude")

    parser.add_argument("--plot-tile", type=str, help="Plot all streets in a given tile file (.bin) in map-style format")
    parser.add_argument("--color-by-speed", action="store_true", help="Color streets by speed limit when plotting a tile")
    parser.add_argument("--print-all-info", action="store_true", help="Print all available street info and annotate it on the plotted map")
    parser.add_argument("--show-coordinates", action="store_true")
    parser.add_argument("--scan-all", action="store_true")

    args = parser.parse_args()

    if args.plot_tile:
        plot_tile_file(args.plot_tile, color_by_speed=args.color_by_speed, print_all_info=args.print_all_info)
    elif args.plot_tile:
        plot_tile_file(args.plot_tile, args.color_by_speed)
    elif args.scan_all:
        scan_all_tiles()
    else:
        if args.lat is None or args.lon is None:
            print("Error: You must supply lat and lon unless using --scan-all.")
        else:
            read_tile(args.lat, args.lon, args.show_coordinates)
