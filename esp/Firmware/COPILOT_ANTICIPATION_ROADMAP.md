# Anticipation Roadmap: Ruptela → ESP32 Copilot

*Evolving the offline speed/street display into a predictive "what's coming" copilot.
Sequenced for a heap-constrained (PSRAM-off) build, WiFi on-demand only, and small
shippable increments.*

> **Context / why this exists.** Today the copilot screen receives **pure NMEA** over the
> Ruptela transparent channel (RS232, GPIO18) and shows current speed, a map-matched street
> name, and the local speed limit. The goal is to make the display *anticipatory* — use
> position + heading + speed (and, if the link allows, CAN-derived RPM/speed/ECO-idle) to
> surface **what's coming**: the next road, the upcoming speed-limit drop, curves ahead.
>
> The central finding of the planning pass (firmware read first-hand + both Ruptela PDFs +
> the Lua skill pack, run through a 4-lens design/judge process) is that **most of the
> anticipation vision needs no CAN and no Lua at all** — the two missing ingredients are
> already on the device and unused:
> - `gps_t.cog` (course over ground) is parsed from every RMC/VTG sentence
>   (`main/nmea_parser.c:270`, `:335`) and then **discarded**.
> - The offline tiles are **full road polylines** (per-segment point list + speed + name,
>   `components/tile_reader/tile_reader.c:157-235`), not point lookups. The road network
>   *ahead* of the vehicle is already computable on-device.
>
> So the predictive product can ship from data we already receive, while the richer
> CAN/RPM track stays gated behind one unconfirmed question about the serial link.

---

## 1. Reality check — what the link can and can't do today

**What arrives on GPIO18 right now (verified):** pure NMEA at 115200 8N1. The parser
(`main/nmea_parser.c`) keys on `$`, frames on the `\n` UART pattern (line 695), validates
per-sentence XOR checksum, and dispatches RMC/GGA/GSA/GLL/VTG. From this we already get and
**use**: `latitude`, `longitude`, `speed`, plus map-matched street + speed limit from
`/sdcard/tiles`.

**What we already receive but throw away:** `gps_t.cog` (course over ground, parsed at
`nmea_parser.c:269-270, 335`), `sats_in_use`, `dop_h`. **This is free anticipation fuel
sitting unused.**

**What we cannot do today:**
- We **cannot** receive Ruptela binary I/O (RPM, CAN speed, fuel, ECO-idle) — the parser
  ignores non-`$` bytes, and the `\n` pattern-detect would be corrupted by binary frames
  containing stray `0x0A`.
- We **do not know** whether the tracker can even emit those values out the RS232
  transparent channel. Lua has **no serial-write API** — its only output is `io.set()` into
  custom slots 1101-1140, and neither the transparent-channel PDF nor Protocol v1.133
  confirms those slots leave the port toward a connected device (vs. server-only).

### The ONE open question — test this before any I/O-track work

> **Do Ruptela I/O record frames — and specifically custom Lua slots 1101-1140 — actually
> appear on the RS232 wire the ESP reads?**

**How to test (Stage 0 below, ~half a day):**
1. **Ruptela configurator:** enable *"Send IO data through RS232"* on the transparent
   channel; note the parser begin/end symbols (`0x7E` is always the terminator).
2. **Confirm tracker model.** Lua exists **only** on HCV5-Lite / Pro5-Lite / Smart5. If it's
   another model, the Lua track is dead before it starts. Also: HCV5-Lite / Pro5-Lite **cap
   RS232 at 9600 baud** — verify the unit sustains 115200, or the project's baud assumption
   breaks.
3. **Lua sentinel (if Lua-capable):** ~10 lines, `io.set(1, 0, hi, lo)` (slot 1101) with an
   incrementing counter every 1000 ms; `luastart`, watch `IO1141` status.
4. **ESP capture:** throwaway build that hex-dumps every **non-NMEA** byte on UART1 to
   `/sdcard/diag.log` via the existing `diag_log_line()`.

**Decision matrix:**

| Wire shows… | Verdict | Consequence |
|---|---|---|
| The 1101 counter | **GREEN-custom** | Full Lua pre-processing path open → cleanest data |
| Standard CAN IDs only (RPM/bSpeed/175), no 1101 | **GREEN-standard** | Consume native IO IDs; no Lua needed |
| No binary frames at all | **RED** | Entire CAN/RPM track is dead on this single wire → NMEA-only anticipation is the product |

Everything in Section 3 below Stage 3 is gated on this result. **Stages 1-2 deliver the
headline anticipation value regardless of the outcome.**

---

## 2. Architecture — inbound data model & framing

### Two integration seams (from the firmware analysis)

- **Seam A (parser-side):** extend `gps_t` in `main/nmea_parser.h:97-116`. Good for data
  that rides NMEA framing.
- **Seam B (shared-state, recommended for new streams):** new vars in `main/ui/vars.c`
  mirroring `current_speed`, registered in `main/ui/ui.c:81`, written from the main loop.
  Decouples new data from the parser entirely.

### Inbound data model (target)

```
struct telemetry {            // populated in main loop, Seam B
  // --- always available (NMEA, today) ---
  float    cog;               // course over ground, deg   [parsed, unused]
  uint8_t  sats; uint8_t hdop_x10;
  // --- derived (Stage 1+, ESP-side, zero link cost) ---
  int32_t  next_speed_limit;  // limit at projected probe point
  char     next_street[64];
  int32_t  next_limit_dist_m;
  // --- gated on Stage 0 GREEN ---
  int32_t  rpm;     uint32_t rpm_rx_us;        // freshness per field
  int32_t  can_speed_kmh; uint32_t can_speed_rx_us;
  int32_t  eco_idle_s;
};
```
Every link-sourced field carries an **ESP receive timestamp**, not record content — the
transparent channel buffers up to 1024 records and can replay stale data after a gap.
Freshness = "did *I* see it recently."

### Serial framing — three viable paths, in risk order

1. **Pseudo-NMEA (lowest risk, preferred if Lua→serial works):** Lua formats values into a
   checksummed proprietary sentence `$PCOPI,rpm,canspd,idle*CS\r\n`. It rides the
   **existing** `\n`-framed parser untouched — add a `STATEMENT_PCOPI` branch +
   `parse_pcopi()` mirroring `parse_vtg`. No binary demux, no second UART, no regression to
   the working GPS path.
2. **Raw byte-wise demux/framer (only if binary frames are required):** replace `\n`
   pattern-detect with a state machine that owns boundaries — collect to `0x7E`, un-escape
   `0x7D` (XOR 0x20), classify `$`→legacy NMEA path *byte-for-byte unchanged*, `##`→IMEI,
   else length-prefixed I/O record (CRC8-validate). Higher regression risk; gate behind a
   Kconfig flag.
3. **Second UART (e.g. GPIO19) + its own task:** physical separation, no demux risk to NMEA.
   L/XL effort; reserve for if binary I/O proves valuable and can't ride pseudo-NMEA.

**Decision rule:** GREEN-custom → path 1 (Lua packs `$PCOPI`). GREEN-standard → path 2 (must
parse native binary records). RED → neither; stay NMEA-only.

### Ruptela-side config shape

- **Configurator:** confirm NMEA-to-RS232 ON (already is); for I/O, enable "Send IO data
  through RS232", lock `0x7E` terminator, decide whether IMEI emission stays off (avoids
  `##` handling).
- **Lua packer (GREEN-custom only):** `timer 1000ms → gps.bSpeed(); io.get(rpmID);
  io.get(175) → io.set()` into custom slots **or** format `$PCOPI` if a serial path is
  confirmed. Compile `.lc` ≤10 kB, min pause ≥100 ms.
- **FMIO IDs:** RPM and most CAN parameter IDs are **not in the supplied PDFs** (FMIODATA
  file referenced but absent). Source each ID before writing the packer; confirm the vehicle
  CAN is physically wired/decoded.

---

## 3. Staged roadmap

Stages 1-2 are the highest-leverage work and depend on **nothing** but data we already
receive. Stage 0 runs in parallel as a bench task. The I/O track (Stage 4+) only opens if
Stage 0 is GREEN.

### Stage 0 — Confirm the link (bench gate, blocking for I/O track only)
- **Goal:** answer the one open question; record GREEN-custom / GREEN-standard / RED.
- **Ruptela:** enable "Send IO data through RS232"; confirm model + sustained baud; deploy
  the `io.set(slot 1101)` counter script.
- **ESP:** throwaway build hex-dumping non-NMEA UART1 bytes to `diag.log` (reuse
  `diag_log_line`).
- **Effort:** S. **Unlocks:** the entire CAN/RPM decision. **Verify:** grep the dump for the
  known counter / standard IDs / any binary frames.

### Stage 1 — Heading-projected lookahead ★ (ship first)
- **Goal:** show the road and speed limit the driver is *about to* enter, not just the one
  underneath.
- **Ruptela:** none.
- **ESP:**
  1. *Sub-step 1a (de-risk, S):* speed-gate + log projected `lat2/lon2` ahead to `diag.log`
     to validate the dead-reckoning math on a real drive — **no UI risk.**
  2. Add `get_speed_and_name_at_lookahead(lat, lon, cog, &spd, street, &dist)` next to
     `get_speed_and_name_at()` in **`components/tile_reader/tile_reader.c`** (note: top-level
     component, not under `main/`). Project forward along `cog`: `dlat = cos(cog)·d/R`,
     `dlon = sin(cog)·d/(R·cos(lat))`. Horizon `d = clamp(speed·4s, 60, 250) m`. **Reuse
     `scan_tile_for_match` unchanged** — it streams points one-at-a-time (no malloc), so this
     is heap-safe.
  3. Call after the existing match (`main/offline_maps.c:~368`); store in new
     `next_street`/`next_speed_limit` shared vars (mirror
     `set_street_name`/`set_var_speed_limit_value`).
  4. **Speed gate:** suppress all cog-derived output below ~15 km/h (cog is noise at
     standstill); freeze last-good prediction.
- **Effort:** M. **Unlocks:** the core anticipation experience with zero link risk.
  **Verify:** drive a route; confirm "next road" resolves to the correct upcoming segment,
  not the parallel/opposite one.

### Stage 2 — Lower-limit-ahead pre-warning
- **Goal:** flash "limit dropping to NN" before the vehicle reaches a school zone / town
  entry.
- **Ruptela:** none.
- **ESP:** in `calculate_threshold()` (`main/dynamic.c`), if `next_speed_limit > 0 &&
  next_speed_limit < current_limit && (horizon/speed) < t_warn`, raise the existing
  `speed_limit_warning_label` / `show_temp_message()`. Pure logic on Stage 1 data.
- **Effort:** S. **Unlocks:** proactive coaching. **Verify:** approach a known limit drop;
  warning fires seconds early.

### Stage 3 — Heading-biased map-matching + heading cue on the arc
- **Goal:** stop wrong-road flips on divided/parallel roads; make the screen feel alive.
- **Ruptela:** none.
- **ESP:**
  - In `scan_tile_for_match` (`components/tile_reader/tile_reader.c`), when two candidate
    segments are within a small distance margin, add a penalty ∝ `|bearing(seg) − cog|`.
    Keep it a **tie-breaker only**, speed-gated >5 km/h so stationary matching still works by
    distance.
  - Drive a previously-idle arc (`screens.c:204-268`) from `cog` or heading-change.
    **Binding choice:** either regenerate the EEZ Studio `.eez` flow to bind a `heading`
    native var, **or** (lower-friction) call `lv_arc_set_value()` directly under `lvgl_lock`
    from `calculation_task`, bypassing the flow engine. The EEZ round-trip is the expensive
    path.
- **Effort:** M. **Unlocks:** matching accuracy + directional feedback. **Verify:** drive a
  divided highway / service-road pair; street name stops flipping to the wrong side.

> **--- I/O track below: gated on Stage 0 = GREEN ---**

### Stage 4 — Land CAN data the lowest-risk way
- **Goal:** get RPM / CAN-speed / ECO-idle onto the device.
- **GREEN-custom path:** Lua packs values into a checksummed `$PCOPI` sentence → add
  `parse_pcopi()` + `gps_t` fields + shared vars. **No binary demux, no second UART** —
  additive to the working parser.
- **GREEN-standard path:** implement the raw demux/framer (Section 2, path 2): own
  `0x7E`/`0x7D` framing, keep NMEA classification byte-identical, CRC8-validate I/O records,
  decode size-classed IO groups (protocol §1.3/1.6), map known IDs to shared vars. L effort,
  higher regression risk — flag-gated bring-up.
- **Ruptela:** deploy the Lua packer (custom) or select the standard IO IDs to emit; source
  FMIO RPM ID first.
- **Effort:** M (custom) / L (standard). **Unlocks:** real engine/CAN telemetry. **Verify:**
  bench — known RPM/idle values appear in shared vars; NMEA path provably unchanged.

### Stage 5 — CAN-speed swap + per-source freshness
- **Goal:** smoother speedometer + trustworthy stale-handling.
- **ESP:** prefer `can_speed_kmh` when `now − can_speed_rx_us < 2s`, else fall back to GPS
  speed (`offline_maps.c:352-359`); show a small source badge. Add per-field freshness in
  `calculation_task` (100 ms) — gray out fields stale past threshold instead of showing
  frozen numbers (defends against buffered-record replay).
- **Effort:** S. **Unlocks:** trust. **Verify:** kill CAN mid-drive; speedometer falls back
  and badge/gray-out reflects it.

### Stage 6 — Engine-aware surfaces & data-format projects (optional)
- RPM tach on the spare arc; ECO-idle glyph. Keep to scalar vars + existing screen
  (BOOT-cycled view) — **avoid new LVGL screens** under PSRAM-off.
- **Separate data-format project:** extend `python/extract_tiles.py` to emit segment
  **node/connectivity topology** — required for true junction-to-junction turn-by-turn (see
  Section 4). Not firmware-only.
- **Effort:** M (tach) + L (exporter). **Unlocks:** full predictive turn cues.

---

## 4. The anticipation engine

**Inputs → output:**
```
position (lat/lon, used)  ┐
heading  (cog, NOW used)  ├─► dead-reckon probe point  ─► tile nearest-segment scan
speed    (horizon scale)  ┘        (60-250 m ahead)         (reuse scan_tile_for_match)
                                          │
            (Stage 4+) rpm/can_speed ─────┼─► enriches confidence / engine-load logic
                                          ▼
                          next_street, next_limit, dist  ─► UI: "limit 50 in 200 m" + arc cue
```

**What the offline tiles give us (verified):** per-segment polylines (`numPoints`, lat/lon
e7, speed, name), streamed without malloc. This is enough for:
- ✅ **Lookahead road/limit** — project a point ahead, match it. Works today.
- ✅ **Within-segment curvature** — bearing change along a single polyline → reliable "curve
  ahead" + left/right sign.

**What the tiles do NOT have:** node IDs / segment-to-segment **topology**.
`scan_tile_for_match` reads each segment independently with no connectivity graph. Therefore:
- ⚠️ **Cross-junction turn-by-turn** ("turn right at the next intersection") is only
  *approximate* via geometric proximity. True junction prediction needs the **exporter
  change** in Stage 6, not just firmware.

**Heading caveats baked into every cog feature:** cog is *retrospective* (where you've been
heading) and *noise at low speed*. Hence the universal **speed gate (>~15 km/h)**, a
**modest horizon (120-200 m)**, and treating lookahead as a **hint, not a hard switch** — at
a sharp fork the probe can land on the wrong branch.

**Latency:** main loop throttles to ~0.9 s (`GPS_PROCESS_PERIOD_US`); at 100 km/h that's
~25-40 m/sample — fine for road/limit lookahead, coarse for tight-curve *timing*. If curve
warnings feel late, run the prediction-only fields on a faster path than the 1 Hz queue.

---

## 5. Risks & open questions

**Blocking / data-link (Stage 0 answers all of these):**
- **Custom Lua slots may be server-only** — no confirmed RS232 emission of 1101-1140. RED
  here kills the entire CAN/RPM/bSpeed track on this single wire.
- **Tracker model** — Lua only on HCV5-Lite / Pro5-Lite / Smart5. HCV5-Lite / Pro5-Lite cap
  RS232 at **9600 baud** vs. the assumed 115200; binary I/O + NMEA may not fit at 9600.
- **FMIO RPM (and other CAN) IDs are not in the supplied PDFs** — source from the FMIODATA
  file; confirm the vehicle CAN is wired/decoded.

**Engineering:**
- **Parser regression** — never mix binary onto the `\n`-pattern UART (`nmea_parser.c:695`);
  use pseudo-NMEA, a dedicated framer that keeps NMEA byte-identical, or a second UART.
  Verify NMEA behavior before/after on the bench.
- **Heap (PSRAM OFF)** — lookahead reuse is safe (streaming reader, stack `nameBuf`, bounded
  scan). Any new LVGL screen or binary ring must be static/bounded. **Measure the existing
  ~5 s heap heartbeat before/after each increment** rather than assuming.
- **EEZ flow binding is opaque** — arcs read a flowState property bound to
  `indicator_threshold`. Binding new vars cleanly = EEZ Studio regeneration; the cheap
  fallback is direct `lv_arc_set_value()` under `lvgl_lock`. Pick deliberately.
- **Tile topology** — confirm the exporter change is in scope before promising intersection
  turn-by-turn.
- **cog validity** — validate the dead-reckoning probe on a real drive (Stage 1a) before any
  UI trusts it.

**Open questions to resolve:**
1. Stage 0 result (GREEN-custom / GREEN-standard / RED)?
2. Exact tracker model + sustained baud?
3. RPM / ECO-idle / CAN-speed FMIO IDs?
4. EEZ regeneration vs. direct `lv_arc` for new bindings — your preference?
5. Lookahead horizon + speed-gate thresholds — tune on real drives.

---

*Relevant seam files:* `main/nmea_parser.{c,h}` (parser, `gps_t`), `main/offline_maps.c`
(main loop ~316-395, event handler 80-121), `components/tile_reader/tile_reader.c` (matcher —
top-level component), `main/dynamic.c` (threshold/warnings), `main/ui/vars.{c,h}` +
`main/ui/ui.c:81` (shared state), `main/ui/screens.c:204-268` (arcs), `python/extract_tiles.py`
(tile exporter / future topology).
