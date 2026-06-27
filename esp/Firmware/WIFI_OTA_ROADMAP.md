# WiFi + OTA de tiles — Roadmap y estado

Documento de continuidad: qué se construyó, las decisiones/gotchas clave, el estado de RAM,
y los próximos pasos. Objetivo general: que cada equipo pueda **actualizar los tiles de mapas
por aire**, sin sacar la SD.

El trabajo se divide en capas independientes:

- **Capa A — Conectividad WiFi + provisioning.** ✅ HECHA (este documento).
- **Capa B — Distribución de mapas** (catálogo de zonas, descarga, flag de updates). ⏳ Pendiente.
- **Capa C — OTA de firmware** (particiones dual-OTA, `esp_https_ota`, rollback). ⏳ Track aparte, futuro.

---

## Capa A — HECHA

### Qué hace
- Las credenciales WiFi viven en **NVS** (namespace `netcfg`: `ssid`, `password`, `base_url`).
- **Pre-carga de fábrica/flota:** si NVS está vacío, se importa una vez `/sdcard/net_config.json`.
- **Self-service:** long-press del botón **BOOT** abre un **portal cautivo** (SoftAP `Mykeego-XXXX`)
  con un **QR** en pantalla. El cliente escanea → el celular se une al AP → se abre la web sola
  (DNS hijack) → elige su red y pone la clave → se guarda en NVS → el equipo **reinicia** y queda
  listo para conectar a demanda.
- **El WiFi es 100% on-demand:** NO se conecta en el boot, NO queda prendido. Sólo se levanta para
  provisioning (y, en la Capa B, para descargar) y se apaga al terminar. El boot arranca rápido y
  el equipo opera offline con los tiles que ya tiene.

### Componentes / archivos
- `components/net_link/` — WiFi STA + store de credenciales en NVS.
  - API: `net_link_import_sd_config()`, `net_link_have_credentials()`, `net_link_set_credentials()`,
    `net_link_connect(timeout)`, `net_link_is_connected()`, `net_link_get_ip/rssi/base_url()`,
    `net_link_stop()`.
  - `connect()`/`stop()` están listos para el patrón on-demand de la Capa B (connect → bajar → stop).
- `components/wifi_portal/` — SoftAP + portal cautivo + DNS hijack.
  - `wifi_portal_run(timeout)` (bloqueante, el llamador reinicia después), `wifi_portal_get_ap_ssid()`.
  - HTML embebido + `GET /scan` (escanea redes) + `POST /save` (guarda en NVS) + catch-all redirect.
- `main/buttons.c` — short-press = mensaje (como antes); **long-press (~3 s)** = pide provisioning
  vía `buttons_take_provisioning_request()`.
- `main/offline_maps.c` — en el boot sólo `net_link_import_sd_config()` (sin conectar). El loop
  principal llama `maybe_run_provisioning()`: muestra el overlay con el QR, corre el portal y
  **reinicia** (guardado o timeout).
- `main/waveshare_amoled_lcd_port.{cpp,h}` — buffers LVGL en **RAM interna** y `LVGL_BUF_HEIGHT = V_RES/8`.
- `partitions.csv` — `factory` agrandada `0x2EE000 → 0x400000` (el stack WiFi hizo crecer la app a ~3,4 MB).
- `sdkconfig` — PSRAM octal habilitada + `CONFIG_LV_USE_QRCODE=y`.

### Gotchas / aprendizajes críticos (NO repetir)
1. **Buffers LVGL NO van en PSRAM.** El flush (`esp_lcd_panel_draw_bitmap`) es DMA asíncrono; si el
   buffer está en PSRAM, cualquier escritura a flash (`esp_wifi_deinit`, NVS) **deshabilita la cache**
   y el DMA-desde-PSRAM falla → `spi transmit color failed` → LVGL colgado → task watchdog. Crasheaba
   en boot y en provisioning. **Solución:** buffers en RAM interna DMA (cache-safe, además
   `CONFIG_SPI_MASTER_ISR_IN_IRAM=y`). PSRAM queda habilitada sólo como headroom general.
2. **PSRAM apagada vs defaults:** el `sdkconfig` activo tenía PSRAM off aunque `sdkconfig.defaults`
   la pide. NO borrar `sdkconfig` para regenerar (defaults no tiene la partition table custom y se
   rompería) — editar en su lugar.
3. **Provisioning siempre reinicia** (con LVGL parqueado tomando `lvgl_lock` antes de `esp_restart`):
   evita el teardown graceful de WiFi compitiendo con LVGL. El reset deja estado limpio.
4. Cambió la partition table → al flashear hay que hacer **flash completo** (no sólo app).
5. **APSTA = un solo radio: STA y SoftAP comparten canal.** El SoftAP arranca en canal 1; si la red
   del cliente está en otro canal, la STA nunca asocia (`auth -> init` en loop, sin handshake). Visto
   en hardware con `Personal-916-2.4GHz`. **Solución (`net_link_sta_join_keep_ap`):** escanear el canal
   real de la red, mover el SoftAP a ese canal (el celular reconecta solo al SSID abierto) y fijar
   `wc.sta.channel` + `scan_method=ALL_CHANNEL_SCAN`. El `WIFI_EVENT_STA_DISCONNECTED` ahora loguea
   `reason=` (201=NO_AP_FOUND ⇒ canal/señal; 15/205/2 ⇒ password).

### Estado de RAM (auditoría con `idf_size.py`)
- RAM interna total (DIRAM): **~334 KB**. Estática usada ~156 KB; **libre para heap ~178 KB**.
- Buffers LVGL (runtime): **~108 KB** → quedan **~70 KB internos** + **~8 MB PSRAM** en operación normal.
- WiFi: ~23 KB estáticos internos (la mayoría código IRAM); el grueso está en flash. Sus buffers
  dinámicos (~60-80 KB) sólo se asignan cuando WiFi está activo (on-demand) y se liberan.
- **Veredicto: uso eficiente.** Las dos decisiones clave (WiFi on-demand + buffers /8 en vez de /4,
  que liberó ~102 KB vs el original) son las correctas.

---

## Capa B — Distribución de mapas (PRÓXIMO)

Diseño acordado (ver charla): un servicio expone un **catálogo de zonas**; el cliente **elige las
zonas**; se descargan los tiles de esas zonas; y cuando un tile de una zona elegida cambia, el equipo
al bootear marca **"hay updates en tus mapas"**.

Contexto útil:
- El set de tiles entero pesa ~390 KB (340 archivos ~1,1 KB) en `/sdcard/tiles/tile_X_Y.bin`.
- `tile_reader.c` abre cada tile con `fopen` por muestra GPS → escribir con `.tmp` + `rename` atómico.
- Generación de tiles offline: `python/extract_tiles.py` (extender para emitir un manifiesto por zona
  con hash/size/version).
- Reusar `net_link_connect()`/`net_link_stop()` (on-demand) y `base_url` de NVS.

Pasos sugeridos para retomar:
1. Definir formato del servicio: catálogo de zonas + manifiesto por zona (lista de tiles con sha256/size + version).
2. Extender el pipeline Python para publicar zonas + manifiestos.
3. Componente nuevo (p.ej. `map_sync`): connect WiFi → bajar manifiesto de las zonas elegidas →
   delta → descargar `.tmp` → verificar → `rename` atómico → stop WiFi. Todo no-bloqueante al core.
4. Selección de zonas por el cliente (definir UX: ¿en el portal web del provisioning? ¿pantalla?).
5. Flag "hay updates" mostrado al boot.

### Progreso (incrementos)
**✅ Incremento 1 — Pipeline Python + servidor de prueba HTTPS (host-side).** Hecho y testeado.
- `python/zones.json` — define zonas por bbox lat/lon (hoy: `caba_oeste`/`caba_centro`/`caba_este`,
  340 tiles repartidos sin solapamiento).
- `python/make_catalog.py` — escanea `tiles/`, asigna cada tile a zona por su centro, computa
  `sha256`+`size`, y emite `tiles/catalog.json` + `tiles/zones/<id>.json`. `version` por zona sube
  SOLO cuando cambia su fingerprint (set de tiles+hashes), persistido en `tiles/versions.json`.
  Integrado al final de `extract_tiles.py` (emit_catalog), o corre standalone.
- `python/map_test_server.py` — HTTPS (self-signed) sirviendo desde `tiles/`:
  `/catalog.json`, `/zones/<id>.json`, `/<tile>.bin`. Flags: `--gen-cert`, `--print-cert`
  (PEM para embeber en firmware), `--corrupt <tile>` (rompe 1 byte → testea rechazo por sha256),
  `--bump-version`. Cert/key en `.gitignore`.
- **Formato (contrato con el firmware):** catalog `{schema, generated, zones:[{id,name,version,tiles,bytes,manifest}]}`;
  manifiesto `{schema, id, version, tiles:[{name, size, sha256, url}]}` donde `url` es relativa al
  `base_url` (= raíz que sirve los tiles, así que `url` == nombre de archivo).
- Verificado: `curl --cacert`, sha256 manifest == archivo, `--corrupt` cambia el sha, bump por-zona aislado.

**🔨 Incremento 2 — `map_sync` fetch de catálogo (read-only en device).** Código escrito,
pendiente build+test en hardware (build va por la extensión VS Code, no hay idf.py CLI).
- `components/map_sync/{map_sync.h,map_sync.c,CMakeLists.txt,Kconfig}` nuevo.
  - `map_sync_fetch_catalog(&zones,&n)` / `map_sync_free_catalog()` — GET `<base_url>/catalog.json`
    por HTTPS (`esp_http_client`), parsea a `map_zone_t[]`. Asume WiFi STA ya conectado.
  - `https_get()` helper con tope de respuesta; verificación TLS SIEMPRE on.
  - `map_sync_selftest()` (gated `CONFIG_MAP_SYNC_BOOT_SELFTEST`): connect→fetch→log zonas→stop.
    Llamado desde `app_main` tras la importación de credenciales.
- Cert: `CONFIG_MAP_SYNC_TEST_CERT` embebe `test_server_cert.pem` (gitignored; generar con
  `map_test_server.py --print-cert`). Sin el flag usa el certificate bundle de mbedTLS.
- **Para testear:** generar cert con la IP LAN de la PC (`--gen-cert --host <IP>`), copiar el PEM al
  componente, `base_url`=`https://<IP>:8443` en NVS (net_config.json), activar
  `MAP_SYNC_TEST_CERT`+`MAP_SYNC_BOOT_SELFTEST`, build+flash, ver log `selftest: catalogo OK, 3 zonas`.

**✅ Incrementos 3–6 — firmware completo. COMPILA Y LINKEA** (verificado con ninja, IDF v5.4.4/GCC14;
`Offline_maps.bin` ~3,75 MB, 10% libre en la partición factory). Pendiente test en hardware.
Gotchas de build resueltos: `%.23s` por `-Werror=format-truncation` en un `%s` de array fijo; enums del
portal renombrados `PW_*`/`PC_*` porque `W_CONNECTING` colisiona con un macro de sistema; escape hex
`\xc3\xb1a` partido (`\xc3\xb1" "a`) para que la `a` no se coma el escape.
- **Incr.3 — `net_link_sta_join_keep_ap()` + sdkconfig.** En `net_link.c`: nueva función que asocia
  STA SIN cambiar el modo (asume APSTA ya levantado por el portal) ni re-init; en fallo NO hace
  teardown (deja el AP arriba). Refactor de `register_handlers()`/`wait_for_ip()` compartidos (el
  camino Capa A de `net_link_connect` quedó intacto salvo usar esos helpers). `sdkconfig`+`.defaults`:
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`.
- **Incr.4 — Portal AJAX (APSTA)** en `wifi_portal.c`: página nueva (red/clave → poll → checkboxes de
  zonas → guardar). Endpoints `POST /wifi` (worker: join STA + fetch catálogo en vivo), `GET /status`,
  `GET /zones`, `POST /save` (persiste zonas elegidas). El catch-all `/*` queda registrado último, así
  los GET específicos matchean primero. Worker con stack 8192 (handshake TLS).
- **Incr.5 — `map_sync_run()`** en `map_sync.c`: por zona baja el manifiesto, delta por `size`,
  descarga a `<tile>.tmp` con verificación sha256 (mbedtls) → `rename` atómico. Flag NVS
  `mapcfg/upd_flag` + `map_sync_updates_pending()`/`clear`. (No persiste versión por zona: la clave NVS
  `zver_<id>` excede el tope de 15 chars; el delta size+sha ya evita redescargas.)
- **Incr.6 — Task de fondo + banner** en `offline_maps.c`: `map_sync_task` (prio 2, core 0, WiFi
  on-demand: `net_link_connect`→`map_sync_run`→`net_link_stop`), disparado en boot vía
  `maybe_start_map_sync()` solo si hay credenciales + zonas elegidas. El loop principal muestra
  `show_temp_message("Mapas actualizados")` cuando el flag está set y lo limpia. La task NUNCA toca LVGL.

**Pendiente (vos):** build+flash por la extensión VS Code (flash COMPLETO: cambió sdkconfig). Probar:
(a) selftest Incr.2; (b) provisioning → portal muestra zonas en vivo, el celular no se cae (go/no-go RAM
del Incr.3); (c) elegir zonas → reboot → la task de fondo baja deltas → banner. Server de prueba:
`python map_test_server.py` con `--corrupt`/`--bump-version`.
(Plan completo: `~/.claude/plans/vamos-a-continuar-con-delightful-wirth.md`.)

**Optimización recomendada para esta capa (TLS):** activar
```
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```
para que los buffers de WiFi/lwip prefieran PSRAM y dejen la RAM interna libre para el handshake de
mbedTLS (~40-50 KB) durante las descargas. Activar y testear junto con las descargas.

---

## Capa C — OTA de firmware (FUTURO, track aparte)
- Tabla de particiones dual-OTA (`ota_0`/`ota_1`/`otadata`) — entra en los 8 MB de flash.
- `esp_https_ota` + rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
- Reusa la conexión WiFi de `net_link`. **No mezclar** con la Capa B.

---

## Cómo probar la Capa A
1. **Boot:** arranca rápido, muestra el mapa, no toca WiFi. `diag.log`: `wifi cfg ok (on-demand...)`
   o `wifi sin credenciales...`.
2. **Provisioning:** long-press BOOT → overlay con QR a pantalla completa → escanear → unir celular →
   portal → elegir red + clave → "WiFi guardado. Reiniciando..." → reinicia.
3. **Pre-carga flota:** `net_config.json` en la SD con NVS vacío → boot importa a NVS.
4. **Diag del QR:** si el QR sale en blanco, ver la línea `qr res=...` en `diag.log`
   (`res != 1` = falló el encode).

> Nota de build: el firmware se compila desde la **extensión ESP-IDF de VS Code** (el `export.sh` de
> CLI no levanta los toolchains del layout EIM). Tras cambios de partición/sdkconfig → flash completo.
