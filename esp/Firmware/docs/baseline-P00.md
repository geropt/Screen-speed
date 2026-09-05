# P00 — Línea base reproducible y dossier de unidad

Revisión: 2026-09-05. Cierra el paquete P00 del
[plan de evolución](plan-evolucion.md). Registra **lo que se verificó en esta
máquina**, con hashes, y separa explícitamente lo que sigue pendiente de tener la
placa delante. Nada de esta página es una medición de hardware.

## 1. Código de referencia

| Dato | Valor |
| --- | --- |
| Commit de referencia | `bfd3b80a7db85f69666a4331bba3cdfad1e85ad7` |
| Mensaje | «Update README.md with Spanish documentation and firmware details» |
| Autor / fecha | geropt, 2026-09-05 09:45:35 -0300 |
| Rama | `original` (existen además `main`, `ios`, `pruebas`) |
| Dirty state al empezar | Sólo `.kiro/` sin trackear; ningún archivo del firmware modificado |

El directorio `build/` que el usuario tenía (Sep 4 20:55) proviene de un árbol
sucio anterior (`project_version = 10.e546331*`) y **no** es la referencia: su
binario mide 1 689 024 B y no corresponde a ningún commit. La referencia P00 es
el build limpio descrito abajo.

## 2. Toolchain y entorno verificados

| Dato | Valor |
| --- | --- |
| ESP-IDF | v5.4.4 en `/Users/g/.espressif/v5.4.4/esp-idf` |
| Activación | `. /Users/g/.espressif/tools/activate_idf_v5.4.4.sh` (instalación estilo EIM) |
| Invocación | `python $IDF_PATH/tools/idf.py …` — **no hay `idf.py` en PATH** |
| Python del venv IDF | `/Users/g/.espressif/tools/python/v5.4.4/venv` |
| Python del sistema | 3.14.4 |
| Toolchain xtensa disponible | `esp-13.2.0_20240530` y `esp-14.2.0_20260121` |
| CMake | 3.30.2 (bundle IDF) |
| Target | `esp32s3` |

Trampa registrada: `. $IDF_PATH/export.sh` **falla** en esta máquina
(«ESP-IDF Python virtual environment … not found», busca
`~/.espressif/python_env/idf5.4_py3.14_env`). El camino que funciona es el script
`activate_idf_v5.4.4.sh`. Cualquier CI o script nuevo debe usar ese.

Dependencias fijadas (`dependencies.lock`, `manifest_hash`
`81b327394785106e44271c614da5a5ac1dd21aadb07aa7c5f240a0f58b61d9da`):
LVGL 8.4.0, `espressif/esp_lcd_touch` 1.2.1, `esp_lcd_touch_ft5x06` 1.1.0~1,
`cmake_utilities` 0.5.3, `esp_lcd_sh8601` 1.0.0 (local).

Nota de discrepancia: el proyecto declara touch **FT5x06** por dependencia,
mientras la documentación de la placa indica **CST9217**
([hardware y memoria](hardware-y-memoria.md)). Se resuelve en P01 con la placa,
no aquí.

## 3. Build limpio de referencia

Ensayado en directorio nuevo, sin tocar el `build/` del usuario:

```sh
cd esp/Firmware
. /Users/g/.espressif/tools/activate_idf_v5.4.4.sh
python $IDF_PATH/tools/idf.py -B build_baseline_p00 build
```

Resultado: `Project build complete`, sin errores.

| Artefacto | Valor |
| --- | --- |
| `Offline_maps.bin` | 1 682 288 B |
| SHA-256 | `efe8f7aed8165128717138e45fa6a48b5b4b99f836cddcd79553903976b22c7d` |
| `sdkconfig` SHA-256 | `ea8d5c5b6fc1faaa615c0c938c08ca8e06f8dee4316d374def2d307cdd030b30` |
| `partitions.csv` SHA-256 | `89ac00234ff56bfec3a65641df11e9e2df657d1070a6bb94dfbc5e42af32ec72` |
| Ocupación del slot | 1 682 288 / 3 145 728 B = **53,5 %** de `ota_0` |

`idf.py size` del mismo build (informe estático del `.map`, no heap dinámico):

| Sección | Usado | % | Total |
| --- | --- | --- | --- |
| Flash Data (`.rodata`) | 1 037 844 B | — | — |
| Flash Code (`.text`) | 531 108 B | — | — |
| DIRAM | 115 319 B | 33,74 % | 341 760 B |
| IRAM | 16 383 B | **99,99 %** | 16 384 B |
| RTC slow / fast | 28 B / 24 B | — | 8 192 B c/u |

Hallazgo para P01: **IRAM está a 1 byte del límite** (`.text` 15 356 B +
`.vectors` 1 027 B). Cualquier función nueva marcada IRAM_ATTR, o activar
`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` sobre más objetos, no entra. Esto es un
límite del build actual, no un margen medido de heap.

### Layout de flash y OTA

`partitions.csv` describe 16 MiB con dos slots de app de 3 MiB (`ota_0`/`ota_1`),
`otadata`, `nvs` 24 KiB, `phy_init` y `storage` FAT de 9,875 MiB. El `sdkconfig`
efectivo declara `CONFIG_ESPTOOLPY_FLASHSIZE_16MB`, coherente con esa tabla.

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` **no** aparece activado en el `sdkconfig`
actual (`# ... is not set`, igual que `CONFIG_APP_ROLLBACK_ENABLE`): hay dos
slots, pero el rollback que P09 necesita todavía no está habilitado ni verificado
en el bootloader instalado.

### `sdkconfig.defaults` no reproducía nada de esto

Hallazgo principal de P00. Comparando clave por clave la configuración generada
sólo desde `sdkconfig.defaults` contra el `sdkconfig` en uso aparecieron **25
divergencias**. Un clon limpio del repositorio no construía el firmware de
referencia, y en varios puntos construía algo inservible:

| Clave | Referencia | Default anterior | Consecuencia |
| --- | --- | --- | --- |
| `PARTITION_TABLE_*` | `partitions.csv` | `partitions_singleapp.csv` | Un solo slot de app: sin geometría A/B para P09 |
| `ESPTOOLPY_FLASHSIZE` | 16 MB | 8 MB | Incompatible con una tabla que suma 16 MiB |
| `NMEA_PARSER_UART_RXD` | 18 | 5 | UART del Ruptela en un pin sin conexión real |
| `COMPILER_OPTIMIZATION_*` | PERF (`-O2`) | DEBUG (`-Og`) | Otro perfil de tamaño y velocidad |
| `ESP_MAIN_TASK_STACK_SIZE` | 10240 | 3584 | Stack de main no ensayado |
| `FATFS_LFN_*` | LFN en heap, 255 | `LFN_NONE` | Nombres 8.3 en la tarjeta |
| `LV_COLOR_16_SWAP` | `y` | apagado | Colores invertidos en el panel QSPI |
| `LV_FONT_MONTSERRAT_12/16` | `y` | apagadas | Además rompía la compilación de las demos LVGL |
| `LV_USE_LOG` | apagado | `y` | `lv_demo_stress.c` de LVGL 8.4.0 falla con `-Werror=format` |
| `PERIPH_CTRL_FUNC_IN_IRAM`, `COMPILER_ORPHAN_SECTIONS_PLACE`, `LOG_COLORS` | `y` | apagados | Divergencias menores de imagen |
| `EXAMPLE_LVGL_PORT_TASK_CORE`, `..._AVOID_TEAR_ENABLE` | — | presentes | Símbolos que ningún Kconfig define y ningún archivo usa |

Corregido en este paquete: `sdkconfig.defaults` ahora declara explícitamente lo
que la referencia usa. Verificación de reproducibilidad:

```sh
python $IDF_PATH/tools/idf.py -B build_defaults_check \
       -D SDKCONFIG=/tmp/sdkconfig_p00_defaults build
```

- Compila sin errores.
- Comparación clave por clave contra el `sdkconfig` de referencia: **una sola
  diferencia**, `CONFIG_IDF_INIT_VERSION` = `"5.4.2"` contra `"5.4.4"`. Es
  metadato: el `sdkconfig` del usuario fue generado con IDF **5.4.2** y hoy se
  compila con 5.4.4. Queda registrado como dato del entorno, no como riesgo
  resuelto.
- Binario resultante: 1 682 288 B, **exactamente el mismo tamaño** que la
  referencia, con **81 bytes de diferencia**, todos dentro de `esp_app_desc`
  (cadena de versión de git y hora de compilación). El código generado es
  idéntico.

Las demos de LVGL (`LV_USE_DEMO_WIDGETS/BENCHMARK/STRESS/MUSIC`) siguen
compilándose porque están en la referencia; **no** se apagaron aquí para no
alterar la línea base. Son candidatas de ahorro de flash en P01, con medición
antes y después.

## 4. Dataset de mapas

| Dato | Valor |
| --- | --- |
| Ubicación en repo | `python/tiles` (sharded por columna de latitud) |
| Archivos | 1 961 tiles `.bin` |
| Tamaño total | 8,0 MiB |
| SHA-256 del conjunto | `0ef05a520b9b156f0602747571d86cc4f98c04abfd9f7bcc7dc6466d9d9a3dbd` (hash de la lista ordenada de hashes por archivo) |
| Generador | `python/extract_tiles.py` (osmium), `TILE_SIZE = 0.003°` |
| Config derivada | `components/tile_reader/tile_config.h`: `TILE_SIZE_E7 30000`, `FILENAME_SCALE 10000` |
| Fuente PBF | `argentina-260819.osm.pbf` (427 MB, en la raíz, fuera de control de versiones útil) |

Formato por tile (little-endian), leído del generador y confirmado contra el
lector: `u32 n_segments`, y por segmento `u16 n_points`,
`n_points × (i32 lat_e7, i32 lon_e7)`, `u16 speed`, `u16 name_len`, `name_len`
bytes UTF-8. Un nombre vacío significa vía anónima; `speed == 0` significa sin
límite etiquetado en OSM.

## 5. Interfaz de usuario

| Dato | Valor |
| --- | --- |
| Fuente EEZ | `eez-studio/speed_monitor/speed_monitor.eez-project` |
| SHA-256 | `19fb8589ba1b67cd721e7eb278add13ebcae8d329a7bb56cd0757a8ee92aa634` |
| Versión de proyecto | `v3`, tipo `lvgl`, LVGL declarado **8.3** |
| Resolución | 466 × 466 |
| Páginas | una sola, `Main` |

Discrepancia registrada: el proyecto EEZ declara LVGL 8.3 y el firmware fija
8.4.0. Regenerar desde EEZ es una operación a controlar en P03 (drift y borrado
de lógica manual), no antes.

No hay baseline visual: **no se tomaron fotos ni capturas de la pantalla**, no se
flasheó nada.

## 6. Evidencia de pruebas ejecutada hoy

### Piloto Starlink (ya existía)

```sh
make -C esp/starlink_pilot/host_tests test
```

`ruptela_io_parser tests passed`, `ruptela_proto tests passed`. Compila con
`-Wall -Wextra -Werror` y ASan/UBSan.

### HUD (creado en este paquete)

`esp/Firmware/host_tests` no existía, pese a estar citado en el plan. Se creó el
arnés y una prueba de caracterización del lector/matcher actual:

```sh
make -C esp/Firmware/host_tests test
```

`tile_reader baseline: 13 fixes match the golden`. Compila `tile_reader.c` y
`tile_cache.c` en el host contra los shims de `host_tests/host_shim`, con
ASan/UBSan limpio sobre el dataset real. Detalle en
[host_tests/README.md](../host_tests/README.md).

Contadores de caché de esa corrida (línea base, no objetivo): 11 fixes reales
producen `hits=1 misses=19 entries=19`, 12 602 B retenidos. Los 19 fallos para 11
fixes son el barrido de 9 vecinos con entradas negativas incluidas.

### Hallazgos que la prueba deja fijados, no aprobados

1. `get_speed_and_name_at()` guarda estado en `static` de función
   (`locked_name`, `locked_speed`, `have_lock`, `locked_anon`) **sin punto de
   reset**. El resultado del fix N depende de los anteriores; el golden sólo vale
   para la secuencia exacta en un proceso nuevo. P04 debe introducir estado
   explícito y reset.
2. La caché de tiles es global de proceso, sin teardown; sus contadores son
   acumulativos.
3. Los tests Python del repositorio no cubren el matcher en C. La afirmación de
   «205 tests» del plan sigue sin corresponder a este código.

## 7. Pendiente: requiere la unidad física

Ninguno de estos puntos puede cerrarse desde esta máquina y quedan abiertos para
P01/P11:

- SKU y revisión impresa de la Waveshare, foto, y si es la variante **-G** con
  GNSS LC76G (decide si GPIO18 se puede reutilizar para UART1 RX).
- Flash y PSRAM **reales** leídos del chip (hoy sólo se conoce lo que declara
  `sdkconfig`), y `esptool flash_id`.
- Controlador de panel físico: CO5300 documentado por el fabricante contra driver
  SH8601 usado; y touch CST9217 contra dependencia FT5x06.
- Modelo, firmware y CFG del Ruptela, conversor RS232, baud efectivo y cadencia
  real de NMEA/IO; capturas autorizadas con IMEI de banco, no el operativo.
- Tarjeta SD concreta (marca, clase, tamaño) y su comportamiento de remount.
- Alimentación usada y su comportamiento en brownout.
- Bootloader realmente instalado y si soporta rollback.

## 8. Cómo reproducir esta línea base

```sh
git checkout bfd3b80a7db85f69666a4331bba3cdfad1e85ad7
cd esp/Firmware
. /Users/g/.espressif/tools/activate_idf_v5.4.4.sh
python $IDF_PATH/tools/idf.py -B build_baseline_p00 build
python $IDF_PATH/tools/idf.py -B build_baseline_p00 size
make -C host_tests test
make -C ../starlink_pilot/host_tests test
```

Para comprobar que los defaults siguen reproduciendo la referencia, sin tocar el
`sdkconfig` del usuario:

```sh
python $IDF_PATH/tools/idf.py -B build_defaults_check \
       -D SDKCONFIG=/tmp/sdkconfig_defaults_check build
```

Los directorios `build*/` están ignorados por git: el binario de referencia se
identifica por su SHA-256, no se versiona.

## 9. Estado del paquete

Cerrado: código, entorno, build limpio, layout, dataset, fuente de UI y evidencia
de pruebas identificados y con hash; defaults corregidos y verificados; arnés de
pruebas del HUD creado y en verde.

No cerrado, y explícitamente no aprobado por «compila»: todo lo de la sección 7,
que necesita la unidad física. P00 no autoriza a dar por buenas mediciones de
memoria, latencia ni comportamiento de periféricos.
