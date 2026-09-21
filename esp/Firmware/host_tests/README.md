# Pruebas en host del firmware del HUD

Arnés creado en P00 ([plan](../docs/plan-evolucion.md),
[línea base](../docs/baseline-P00.md)). Permite compilar y ejercitar componentes
del firmware en la máquina de desarrollo, con ASan/UBSan, sin placa.

```sh
make test            # compila y corre todo
make update-golden   # regenera los goldens de caracterización
make clean
```

El dataset por defecto es `python/tiles` del repositorio. Para usar otro:

```sh
make DATASET_ROOT=/ruta/que/contiene/tiles test
```

Para ver los logs de los componentes: `HOST_LOG_LEVEL=4 ./test_tile_reader_baseline`
(0 silencioso por defecto, 5 verbose).

## Cómo funciona

`host_shim/` contiene reemplazos mínimos de headers de ESP-IDF
(`esp_log.h`, `esp_heap_caps.h`, `esp_err.h`, `esp_system.h`, `esp_vfs_fat.h`) y
una versión de `sd_manager.h` que sombrea la del dispositivo para que
`MOUNT_POINT` apunte a un directorio del host en lugar de `/sdcard`. El
`Makefile` pasa esa ruta como `HOST_MOUNT_POINT`, porque `TILE_PATH` se arma por
concatenación de literales.

## Qué prueba y qué NO prueba

Prueba: lógica de parseo, geometría, decisión y manejo de errores del código C
compilado, con detección de UB y de accesos inválidos.

**No** prueba, y no debe usarse para afirmarlo:

- Capacidades de memoria. En el host `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)` es
  un `malloc` común: no hay PSRAM, ni DMA, ni fragmentación real, ni el límite de
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`.
- Tiempos. El host es órdenes de magnitud más rápido; cualquier meta de latencia
  del plan se mide en la placa.
- Ciclo de vida de la SD: montaje, extracción, remount, tarjeta lenta o EIO.
- Concurrencia FreeRTOS, prioridades, colas, watchdog o stacks.
- Cualquier cosa del panel, touch, radio o UART físico.

## Pruebas actuales

### `test_tile_reader_baseline`

Caracterización del lector/matcher **tal como está hoy**, previo a P04. Recorre
una secuencia fija de 13 fixes sobre el dataset y compara la salida contra
`fixtures/tile_reader_baseline.golden`.

No es una prueba de exactitud: registra lo que el código responde, incluso si es
discutible. Un cambio en el golden sólo es aceptable acompañado del commit que
explica por qué el comportamiento cambió, según la regla de equivalencia de P04.

Dos limitaciones vienen del código bajo prueba, no del arnés:

1. `get_speed_and_name_at()` mantiene estado en `static` de función sin reset, así
   que el resultado de cada fix depende de los anteriores. El golden vale para esa
   secuencia, ejecutada en un proceso nuevo.
2. La caché de tiles es global de proceso y sin teardown; sus contadores son
   acumulativos por corrida.

Ambas son parte de lo que P04 debe corregir, con tests que expliquen el cambio.
