#!/usr/bin/env python3
"""Empaqueta el directorio de tiles v1 en una cápsula consultable por rangos.

Requisito central de P07: **los payloads se copian sin cambiar un byte**. La cápsula
es un contenedor, no un formato nuevo de tile. El matcher recibe exactamente los
mismos bytes que recibía del directorio, y eso se verifica con
esp/Firmware/host_tests/test_capsule_equivalence.c contra el golden de P00.

Uso:
    python3 pack_capsule.py tiles/ mapa.cap [--region "AMBA"] [--content-version 1]

Estructura generada:
    [cabecera 64 B][manifiesto 156 B][índice ordenado][payloads v1 tal cual]
"""

import argparse
import os
import re
import struct
import sys
import time
import zlib

MAGIC = b"MKGOCAPS"
HEADER_SIZE = 64
MANIFEST_SIZE = 156
INDEX_ENTRY_SIZE = 24
CONTAINER_VER = 1
READER_MIN_VER = 1
REGION_MAX = 64
SIG_MAX = 64

TILE_RE = re.compile(r"^tile_(-?\d+)_(-?\d+)\.bin$")


def read_tile_config(config_path):
    """Lee TILE_SIZE_E7 y FILENAME_SCALE del header que genera extract_tiles.py.

    Se leen del archivo en lugar de hardcodearse: si el generador cambia la
    geometría, la cápsula tiene que declarar la que realmente corresponde.
    """
    tile_size_e7 = None
    filename_scale = None
    with open(config_path, "r", encoding="utf-8") as f:
        for line in f:
            m = re.match(r"#define\s+TILE_SIZE_E7\s+(\d+)", line)
            if m:
                tile_size_e7 = int(m.group(1))
            m = re.match(r"#define\s+FILENAME_SCALE\s+(\d+)", line)
            if m:
                filename_scale = int(m.group(1))
    if tile_size_e7 is None or filename_scale is None:
        raise SystemExit(f"no se pudieron leer TILE_SIZE_E7/FILENAME_SCALE de {config_path}")
    return tile_size_e7, filename_scale


def collect_tiles(tiles_dir, filename_scale):
    """Devuelve [(lat_e7, lon_e7, ruta)] ordenado por (lat, lon)."""
    found = []
    for root, _dirs, files in os.walk(tiles_dir):
        for name in files:
            m = TILE_RE.match(name)
            if not m:
                continue
            lat_name = int(m.group(1))
            lon_name = int(m.group(2))
            # El nombre guarda la coordenada dividida por FILENAME_SCALE; el índice
            # de la cápsula guarda el origen en 1e7, que es lo que consulta el
            # matcher.
            found.append((lat_name * filename_scale,
                          lon_name * filename_scale,
                          os.path.join(root, name)))
    found.sort(key=lambda t: (t[0], t[1]))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tiles_dir")
    ap.add_argument("output")
    ap.add_argument("--region", default="sin-nombre")
    ap.add_argument("--content-version", type=int, default=1)
    ap.add_argument("--tile-config",
                    default="../esp/Firmware/components/map_match/include/tile_config.h")
    args = ap.parse_args()

    tile_size_e7, filename_scale = read_tile_config(args.tile_config)
    tiles = collect_tiles(args.tiles_dir, filename_scale)
    if not tiles:
        raise SystemExit(f"no se encontraron tiles en {args.tiles_dir}")

    region = args.region.encode("utf-8")[: REGION_MAX - 1]
    if len(region) != len(args.region.encode("utf-8")):
        print("aviso: el nombre de region se truncó", file=sys.stderr)

    index_offset = HEADER_SIZE + MANIFEST_SIZE
    index_size = len(tiles) * INDEX_ENTRY_SIZE
    payload_region_offset = index_offset + index_size

    # Primera pasada: leer payloads, calcular offsets y CRC.
    entries = []
    payloads = []
    cursor = payload_region_offset
    for lat_e7, lon_e7, path in tiles:
        with open(path, "rb") as f:
            data = f.read()
        if not data:
            print(f"aviso: {path} está vacío, se omite", file=sys.stderr)
            continue
        entries.append((lat_e7, lon_e7, cursor, len(data), zlib.crc32(data) & 0xFFFFFFFF))
        payloads.append(data)
        cursor += len(data)

    payload_region_len = cursor - payload_region_offset

    index_blob = b"".join(
        struct.pack("<iiQII", lat, lon, off, ln, crc)
        for (lat, lon, off, ln, crc) in entries
    )
    assert len(index_blob) == len(entries) * INDEX_ENTRY_SIZE
    index_crc = zlib.crc32(index_blob) & 0xFFFFFFFF

    manifest = bytearray(MANIFEST_SIZE)
    manifest[0:len(region)] = region
    struct.pack_into("<IIII", manifest, 64,
                     args.content_version, tile_size_e7, filename_scale, len(entries))
    struct.pack_into("<Q", manifest, 80, int(time.time()))
    struct.pack_into("<I", manifest, 88, index_crc)
    # signature[64] queda en cero: firmarla es trabajo de P10.

    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    struct.pack_into("<HHI", header, 8, CONTAINER_VER, READER_MIN_VER, 0)
    struct.pack_into("<QI", header, 16, HEADER_SIZE, MANIFEST_SIZE)
    struct.pack_into("<QI", header, 28, index_offset, len(entries))
    struct.pack_into("<HH", header, 40, INDEX_ENTRY_SIZE, 0)
    struct.pack_into("<QQ", header, 44, payload_region_offset, payload_region_len)
    struct.pack_into("<I", header, 60, zlib.crc32(bytes(header[:60])) & 0xFFFFFFFF)

    with open(args.output, "wb") as out:
        out.write(header)
        out.write(manifest)
        out.write(index_blob)
        for data in payloads:
            out.write(data)          # sin transformar: mismos bytes que el v1

    total = os.path.getsize(args.output)
    print(f"cápsula: {args.output}")
    print(f"  celdas:            {len(entries)}")
    print(f"  región de payloads {payload_region_len} B")
    print(f"  índice             {index_size} B en {index_offset}")
    print(f"  total              {total} B")
    print(f"  crc del índice     {index_crc:08x}")


if __name__ == "__main__":
    main()
