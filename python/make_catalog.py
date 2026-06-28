"""
Genera el catalogo de zonas + manifiestos por zona a partir de los tiles ya
escritos en tiles/. Salida (servida luego por map_test_server.py):

  tiles/catalog.json          -> { schema, generated, zones: [ {id,name,version,tiles,bytes,manifest} ] }
  tiles/zones/<id>.json       -> { schema, id, version, tiles: [ {name,size,sha256,url} ] }
  tiles/versions.json         -> estado interno: { <id>: {version, fingerprint} }

El `version` de una zona sube SOLO cuando cambia su set de tiles o algun hash
(via fingerprint), para no disparar el flag "mapas actualizados" sin cambios reales.

Reusa el mismo esquema de nombres que extract_tiles.py (tile_<fx>_<fy>.bin), donde
fx = origin_lat_e7 // FILENAME_SCALE. Se puede correr standalone sobre los tiles
existentes o llamarse desde extract_tiles.py (emit_catalog).
"""
import os
import re
import json
import hashlib
import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TILES_DIR = os.path.join(HERE, "tiles")
DEFAULT_ZONES_CFG = os.path.join(HERE, "zones.json")

# Debe coincidir con extract_tiles.py / tile_config.h
TILE_SIZE_E7 = 30000
FILENAME_SCALE = 10000

TILE_RE = re.compile(r"^tile_(-?\d+)_(-?\d+)\.bin$")


def tile_center(fx, fy):
    """Centro geografico (lat, lon) de un tile a partir de sus coords de filename."""
    origin_lat_e7 = fx * FILENAME_SCALE
    origin_lon_e7 = fy * FILENAME_SCALE
    lat = (origin_lat_e7 + TILE_SIZE_E7 / 2) / 1e7
    lon = (origin_lon_e7 + TILE_SIZE_E7 / 2) / 1e7
    return lat, lon


def in_bbox(lat, lon, bbox):
    min_lat, min_lon, max_lat, max_lon = bbox
    return min_lat <= lat <= max_lat and min_lon <= lon <= max_lon


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def emit_catalog(tiles_dir=DEFAULT_TILES_DIR, zones_cfg_path=DEFAULT_ZONES_CFG):
    with open(zones_cfg_path, "r", encoding="utf-8") as f:
        zones_cfg = json.load(f)["zones"]

    # Indexar todos los tiles del disco (name -> {size, sha256, center})
    all_tiles = {}
    for fn in os.listdir(tiles_dir):
        m = TILE_RE.match(fn)
        if not m:
            continue
        fx, fy = int(m.group(1)), int(m.group(2))
        path = os.path.join(tiles_dir, fn)
        all_tiles[fn] = {
            "size": os.path.getsize(path),
            "sha256": sha256_file(path),
            "center": tile_center(fx, fy),
        }

    # Estado de versiones previo
    versions_path = os.path.join(tiles_dir, "versions.json")
    try:
        with open(versions_path, "r", encoding="utf-8") as f:
            versions = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        versions = {}

    zones_dir = os.path.join(tiles_dir, "zones")
    os.makedirs(zones_dir, exist_ok=True)

    catalog_zones = []
    for z in zones_cfg:
        zid, zname, bbox = z["id"], z["name"], z["bbox"]
        tiles = []
        total_bytes = 0
        for fn, info in sorted(all_tiles.items()):
            lat, lon = info["center"]
            if in_bbox(lat, lon, bbox):
                tiles.append({
                    "name": fn,
                    "size": info["size"],
                    "sha256": info["sha256"],
                    # url relativa al base_url (= raiz que sirve el test server, donde viven los tiles)
                    "url": fn,
                })
                total_bytes += info["size"]

        # Fingerprint del contenido de la zona (set de tiles + hashes)
        fp_src = "".join(t["name"] + ":" + t["sha256"] for t in tiles)
        fingerprint = hashlib.sha256(fp_src.encode()).hexdigest()

        prev = versions.get(zid, {})
        if prev.get("fingerprint") == fingerprint:
            version = prev.get("version", 1)
        else:
            version = prev.get("version", 0) + 1
        versions[zid] = {"version": version, "fingerprint": fingerprint}

        manifest = {
            "schema": 1,
            "id": zid,
            "version": version,
            "tiles": tiles,
        }
        with open(os.path.join(zones_dir, zid + ".json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=2)

        catalog_zones.append({
            "id": zid,
            "name": zname,
            "version": version,
            "tiles": len(tiles),
            "bytes": total_bytes,
            "manifest": "zones/" + zid + ".json",
        })
        print(f"zone {zid:14s} v{version}  {len(tiles):3d} tiles  {total_bytes:6d} B")

    catalog = {
        "schema": 1,
        "generated": datetime.datetime.now(datetime.timezone.utc)
            .replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "zones": catalog_zones,
    }
    with open(os.path.join(tiles_dir, "catalog.json"), "w", encoding="utf-8") as f:
        json.dump(catalog, f, ensure_ascii=False, indent=2)
    with open(versions_path, "w", encoding="utf-8") as f:
        json.dump(versions, f, ensure_ascii=False, indent=2)

    print(f"Wrote catalog.json with {len(catalog_zones)} zones")
    return catalog


if __name__ == "__main__":
    import sys
    tiles_dir = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_TILES_DIR
    emit_catalog(tiles_dir=tiles_dir)
