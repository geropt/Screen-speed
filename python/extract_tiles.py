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


# -----------------------------
# DEFAULT SPEED LIMITS (km/h)
# -----------------------------
# When a way has no "maxspeed" tag we fall back to a sensible default
# based on the OSM highway type. Values follow the Argentine traffic law
# (Ley Nacional de Tránsito 24.449) for the Buenos Aires metro area:
#   - calles / residencial: 40
#   - avenidas (primary/secondary): 60
#   - semiautopistas: 100/120
#   - autopistas: 130
#   - calles de convivencia (living_street): 20
DEFAULT_SPEEDS = {
    "motorway":        130,
    "motorway_link":    80,
    "trunk":           120,
    "trunk_link":       60,
    "primary":          60,
    "primary_link":     40,
    "secondary":        60,
    "secondary_link":   40,
    "tertiary":         50,
    "tertiary_link":    40,
    "unclassified":     40,
    "residential":      40,
    "living_street":    20,
    "service":          20,
    "road":             40,
    "pedestrian":       10,
    "track":            20,
    "busway":           40,
    "bus_guideway":     40,
}
# Generic fallback when the highway type is unknown / not listed above.
DEFAULT_SPEED_FALLBACK = 40


# -----------------------------
# DRIVABLE ROAD TYPES
# -----------------------------
# Only roads a vehicle can actually drive on are written to the tiles.
# Pedestrian/cycle-only ways (footway, steps, path, cycleway, etc.) are
# skipped: they are not relevant to a speed monitor and would pollute the
# map-matching with bad candidates.
DRIVABLE_HIGHWAYS = {
    "motorway", "motorway_link",
    "trunk", "trunk_link",
    "primary", "primary_link",
    "secondary", "secondary_link",
    "tertiary", "tertiary_link",
    "unclassified", "residential", "living_street",
    "service", "road", "busway", "bus_guideway", "track",
}

# Human-readable placeholder name (Spanish) used when a drivable way has no
# usable name tag. The firmware skips segments with an empty name, so giving
# them a label makes the road matchable and still shows something sensible.
PLACEHOLDER_NAMES = {
    "motorway":        "Autopista",
    "motorway_link":   "Acceso autopista",
    "trunk":           "Semiautopista",
    "trunk_link":      "Acceso",
    "primary":         "Avenida",
    "primary_link":    "Acceso",
    "secondary":       "Avenida",
    "secondary_link":  "Acceso",
    "tertiary":        "Calle",
    "tertiary_link":   "Acceso",
    "unclassified":    "Calle",
    "residential":     "Calle",
    "living_street":   "Calle",
    "service":         "Calle de servicio",
    "road":            "Calle",
    "busway":          "Carril bus",
    "bus_guideway":    "Carril bus",
    "track":           "Camino",
}
PLACEHOLDER_FALLBACK = "Calle"


def resolve_name(tags, highway):
    """Pick the best available name for a way, with sensible fallbacks."""
    for key in ("name", "name:es", "official_name", "ref", "alt_name", "loc_name"):
        val = tags.get(key)
        if val:
            return val
    # No real name: use a readable placeholder based on the road class so
    # the segment is still matchable by the firmware.
    return PLACEHOLDER_NAMES.get(highway, PLACEHOLDER_FALLBACK)


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


# -------- OSM Handler --------
class RoadHandler(osmium.SimpleHandler):
    def __init__(self, writer):
        super().__init__()
        self.writer = writer

    def way(self, w):
        if "highway" not in w.tags:
            return

        highway = w.tags.get("highway")

        # -------- keep only drivable roads --------
        if highway not in DRIVABLE_HIGHWAYS:
            return

        # -------- speed limit --------
        speed_raw = w.tags.get("maxspeed", "0")
        try:
            speed = int(speed_raw.replace(" mph", ""))

            if "mph" in speed_raw:
                speed = int(speed * 1.60934)
        except:
            speed = 0

        # -------- fill missing speed from highway type --------
        # Many OSM ways (specially in Argentina) have no "maxspeed" tag.
        # Assign a default based on the road class so the firmware always
        # has a usable speed limit instead of 0.
        if speed <= 0:
            speed = DEFAULT_SPEEDS.get(highway, DEFAULT_SPEED_FALLBACK)

        # -------- street name (with fallbacks + placeholder) --------
        street_name = resolve_name(w.tags, highway)

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
