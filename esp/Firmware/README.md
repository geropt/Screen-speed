# ESP32 OSM Street-Matching & Speed-Limit Detection Firmware

This firmware allows an ESP32S3 running ESP-IDF to determine:

- The street name
- The speed limit
- The nearest road segment

…based solely on pre-generated OpenStreetMap (OSM) road tiles stored on an SD-card (under "/tiles" directory).
No internet connection or external cloud service is required.

The firmware uses a map-matching algorithm based on polyline geometry and a Haversine distance calculation to find which road segment the GPS coordinate lies closest to.

## 1. Project Overview

This project implements an embedded offline mapping engine for the ESP32.
A Python script is used to extract OSM road data and produce compact binary tile files (tile_x_y.bin).
The ESP32 loads only the required tiles from an SD card, matches the current GPS coordinate to the correct road segment, and returns the:

- Street name
- Speed limit (km/h)
- Matching distance (meters)

This enables real-time speed-limit detection for dashboards, HUDs, bicycle computers, dashcams, etc.

## 2. Core Concept

The firmware is built on three key ideas:

### 2.1 Geographic Tiling

The world is divided into fixed-size tiles:

```
Tile size: 0.003° latitude × 0.003° longitude  (≈ 330 m × 270 m)
```
Given a GPS coordinate:

```
lon / TILE_SIZE → tile_x
lat / TILE_SIZE → tile_y
```
The firmware loads only the tile the user is currently in, plus 8 neighbor tiles.
This dramatically reduces memory usage and SD-card accesses.

### 2.2 Polyline Representation of Roads

Each road segment in a tile is stored as:

- Number of points
- A sequence of encoded coordinates (lat/lon × 1e7)
- Speed limit (uint16)
- Street name (UTF-8 string)

Roads are stored as polylines, e.g.:
```
P0 → P1 → P2 → … → Pn
```
This is identical to how OSM defines ways/highways.

### 2.3 Street Matching via Minimum Distance

To determine the street you are on:

1) Each tile’s road polylines are loaded into memory.
2) For each segment Pi → Pi+1:
    - Compute the nearest projection of your GPS point onto the segment.
    - Compute the Haversine distance to that projected point (accurate to earth’s curvature).
3) The segment with the smallest distance is the best match.
4) The associated street name and speed limit are returned.

Segments without names are ignored to prevent matching service roads, parking aisles, or broken OSM entries.

## 3. Project Structure

```
/main
   /ui                  # GUI related files
   offline_maps.c       # Main application
/components
   /tile_reader
        tile_reader.c      # Core matching logic
        tile_reader.h
   /sd_card                # SD card mount driver
```
Tile files are stored on SD card under:
```
/sdcard/tiles/tile_x_y.bin
```

## 4. Tile File Format

Each tile_x_y.bin file contains:

| Field |	Type |	Description |
| -- | -- | -- |
| segment_count	| uint32	| Number of road polylines |
| For each segment:	| — |	— |
| num_points	| uint16	| Number of points in the polyline |
| points[]	| int32 pair	| Lat/Lon × 1e7 |
| speed_limit	| uint16	| Speed limit km/h |
| name_length	| uint16	| Length of street name |
| name	| bytes	| UTF-8 street name string |

Designed for:

- Fast sequential reads
- Minimal overhead
- Low RAM usage

## 5. How the Firmware Operates

#### Step 1 — GPS Input
The firmware receives a GPS latitude and longitude, e.g.:
```
float gpsLat = 41.6488853f;
float gpsLon = -87.5224172f;

NOTE: This data would be received from NMEA compatible GPS tracker
```
#### Step 2 — Tile Detection
```
// using multiplication operation rather than division as it is faster
// replacing value of "TILE_SIZE" from "0.01" to "100.0"

int tx = lon * TILE_SIZE;
int ty = lat * TILE_SIZE;
```
The firmware attempts to read:
```
tile_tx_ty.bin
```
If necessary, it then searches the 8 neighboring tiles.

#### Step 3 — Polyline Matching with Haversine Distance

For every tile:

- Load points
- Compute segment-distance to GPS point
- Keep the smallest named segment

Uses:
```
distance_point_to_segment_haversine()
```
This returns the real earth-surface distance in meters.

#### Step 4 — Return Results

Example output:
```
Street: 136th Street
Speed limit: 48 km/h
Match distance: 2.8 m
```
The application then:

- Displays the name on screen
- Updates the speed-limit indicator

## 6. Building & Flashing the Firmware (ESP-IDF v5.4.2)
Install ESP-IDF

Follow official installation instructions:
https://docs.espressif.com/projects/esp-idf/en/latest/

Configure the project, build and flash it.
```
idf.py build
idf.py flash monitor
```

## 7. How to Use the Firmware

1) Extract OSM data to tiles using the Python tool:

- Make sure python is installed in your computer
- Navigate to python folder and run following command
```
python launcher.py <osm_pbf_file_name>

NOTE: PBF files should be placed in "pbf_files" folder
```
2) Copy the resulting tiles/ folder to the SD card root.
3) Insert SD card into the ESP32 system.
4) Power the unit — the firmware will:

    - Read GPS
    - Locate tile
    - Match road
    - Output speed limit and street name

5) If no road is found within the loaded tiles, it returns:
```
No street data found in this area.
```

## 8. Performance Characteristics

- Matching time: 2–8 ms per tile
- SD card seeks: Extremely low (sequential reads)
- RAM usage: Minimal (points loaded per segment)
- Accuracy: ~1–4 meters depending on polyline resolution