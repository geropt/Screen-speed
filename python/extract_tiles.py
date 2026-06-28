import osmium
import struct
import os
from math import floor

# -----------------------------
# CONFIGURATION
# -----------------------------
TILE_SIZE = 0.003      # degrees (330 * 270 m approx)
OUT_DIR = "tiles/"
CONFIG_FILE_PATH = "../esp/Firmware/components/tile_reader/tile_config.h"
os.makedirs(OUT_DIR, exist_ok=True)


# -------- Compute tile index --------
# always use floor to ensure proper tile generation
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


# ---- A writer that stores entries per tile ----
class TileWriter:
    def __init__(self):
        self.tiles = {}  # key = (x,y), value = list of segments

    def add_segment(self, coords, speed, name):
        seen_tiles = set()
        for lat, lon in coords:
            seen_tiles.add(tile_id(lat, lon))
        for t in seen_tiles:
            self.tiles.setdefault(t, []).append((coords, speed, name))

    def write_all(self):
        for (tx, ty), segments in self.tiles.items():
            tx_name = filename_coord(tx)
            ty_name = filename_coord(ty)
            filename = (f"{OUT_DIR}/tile_{tx_name}_{ty_name}.bin")
            config_file = (f"{CONFIG_FILE_PATH}")
            with open(filename, "wb") as f:
                
                # number of road segments in this tile
                f.write(struct.pack("<I", len(segments)))
                
                for coords, speed, name in segments:

                    # ---- number of points ----
                    f.write(struct.pack("<H", len(coords)))

                    # ---- points ----
                    for lat, lon in coords:
                        f.write(struct.pack("<ii", int(lat*1e7), int(lon*1e7)))

                    # ---- speed limit ----
                    f.write(struct.pack("<H", speed))

                    # ---- street name ----
                    if name is None:
                        name = ""
                    name_bytes = name.encode("utf-8")

                    f.write(struct.pack("<H", len(name_bytes)))
                    f.write(name_bytes)

            print(f"Wrote {filename} with {len(segments)} segments")

        with open(config_file, "w") as f:
            f.write(f"#pragma once\n\n")
            f.write(f"#define TILE_SIZE_E7 {TILE_SIZE_E7}\n")
            f.write(f"#define FILENAME_SCALE {FILENAME_SCALE}\n")
            print(f"Wrote {config_file}")

        # ---- catalogo de zonas + manifiestos (Capa B) ----
        try:
            from make_catalog import emit_catalog
            emit_catalog(tiles_dir=OUT_DIR.rstrip("/"))
        except Exception as e:
            print(f"WARN: no se pudo generar el catalogo de zonas: {e}")


# -------- OSM Handler --------
class RoadHandler(osmium.SimpleHandler):
    def __init__(self, writer):
        super().__init__()
        self.writer = writer

    def way(self, w):
        if "highway" not in w.tags:
            return
        
        # -------- speed limit --------
        speed_raw = w.tags.get("maxspeed", "0")
        try:
            speed = int(speed_raw.replace(" mph", ""))

            if "mph" in speed_raw:
                speed = int(speed * 1.60934)
        except:
            speed = 0

        # -------- street name --------
        street_name = w.tags.get("name")

        # -------- coordinates --------
        coords = [(n.lat, n.lon) for n in w.nodes]
        
        if len(coords) >= 2:
            self.writer.add_segment(coords, speed, street_name)


# ---------- MAIN ----------
if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print("Usage: python extract_tiles_with_names.py input.osm.pbf")
        exit(1)

    tw = TileWriter()
    handler = RoadHandler(tw)
    handler.apply_file(sys.argv[1], locations=True)
    tw.write_all()
