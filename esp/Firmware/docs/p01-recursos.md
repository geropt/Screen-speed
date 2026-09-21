# P01 — Servicios de placa, memoria y display bajo control

Revisión: 2026-09-05. Cierra la parte de P01 que se puede hacer sin la unidad
física y deja explícito lo que falta. Complementa la
[línea base P00](baseline-P00.md) y el [plan](plan-evolucion.md).

**Este documento no aprueba P01.** El plan exige «tabla de mediciones
antes/después, colores/áreas correctos, ausencia de fugas al apagar/encender y
margen interno demostrado». Nada de eso se puede obtener sin flashear la placa.
Lo que sí está hecho: la extracción, la instrumentación que producirá esos
números, y la parametrización que permitirá comparar.

## 1. Qué cambió

| Antes | Ahora |
| --- | --- |
| Pines, geometría y secuencia del panel dentro de `main/waveshare_amoled_lcd_port.{h,cpp}` | `components/board_waveshare_175`, con los mismos valores |
| Bus I2C instalado por el bloque de touch, compilado fuera con `USE_TOUCH 0` | `board_i2c`, dueño único, con sondeo que distingue ausencia de error de bus |
| Filas de buffer fijas en `LCD_V_RES / 4` | `CONFIG_BOARD_LVGL_BUF_ROWS`, opciones 116/64/48/32, default 116 |
| Sin instrumentación | `components/res_metrics`: heap, stacks, latencias por canal y contadores de error |
| Touch con `#define USE_TOUCH 0` | `CONFIG_BOARD_ENABLE_TOUCH`, apagado por defecto |
| IMU y audio sin mención | `CONFIG_BOARD_ENABLE_IMU` y `CONFIG_BOARD_ENABLE_AUDIO`, apagados por defecto |

La secuencia de inicialización del panel se movió byte por byte: mismos comandos,
mismo orden, mismos retardos de 600 ms en 0x11 y 0x2B. No se cambió el driver
SH8601 ni el reloj del panel ni LVGL, como pide el plan.

### El dueño único del bus I2C tuvo que ser real

`TouchDrvCST92xx::begin(i2c_port_t, ...)` de SensorLib llama a
`i2c_param_config()` y `i2c_driver_install()` por su cuenta
(`components/SensorLib/src/SensorCommon.tpp:218`). Usar esa variante hubiera
dejado dos dueños del mismo bus: el segundo `install` falla y el resultado
depende de que los parámetros coincidan por casualidad.

Se usa en cambio la variante por callbacks,
`begin(addr, readRegCallback, writeRegCallback)`, con
`board_i2c_read_regs`/`board_i2c_write_regs`. El driver del periférico queda sin
acceso a la instalación del bus, que es la única forma de que el dueño único no
sea una declaración de intenciones.

## 2. Comparación estática medida

Todo lo de esta sección sale de `idf.py size` sobre el binario final. Es informe
del `.map`, **no** heap dinámico: el plan lo advierte explícitamente.

### Configuración por defecto contra la línea base P00

| Sección | P00 | P01 | Δ |
| --- | --- | --- | --- |
| Binario de app | 1 682 288 B | 1 671 280 B | **−11 008 B** |
| Flash Code (`.text`) | 531 108 B | 523 700 B | −7 408 B |
| Flash Data (`.rodata`) | 1 037 844 B | 1 035 796 B | −2 048 B |
| DIRAM total | 115 319 B | 114 235 B | **−1 084 B** |
| `.bss` | 18 512 B | 18 984 B | +472 B |
| `.data` | 15 668 B | 15 636 B | −32 B |
| IRAM | 16 383 B | 16 383 B | **0** |

Lectura honesta de esos números:

- El ahorro de flash y de DIRAM `.text` **no** viene de optimizar nada: viene de
  que el driver de touch de SensorLib y su instancia global dejaron de compilarse
  al pasar el touch a opt-in. Si mañana se habilita el touch, el costo vuelve.
- Los +472 B de `.bss` son el estado estático de `res_metrics` y `board_i2c`.
  Instrumentar cuesta eso y no crece con el uso: no hay asignaciones dinámicas ni
  tareas nuevas.
- IRAM queda **exactamente igual**, a 1 byte del límite. Fue un requisito de
  diseño: ninguna función nueva se marcó `IRAM_ATTR`, incluido el callback de
  DMA del panel.

### Costo de habilitar el touch

| Sección | Touch apagado | Touch encendido | Δ |
| --- | --- | --- | --- |
| Binario de app | 1 671 280 B | 1 696 288 B | +25 008 B |
| Flash Code | 523 700 B | 542 924 B | +19 224 B |
| Flash Data | 1 035 796 B | 1 039 916 B | +4 120 B |
| DIRAM total | 114 235 B | 116 103 B | +1 868 B |

Verificado que compila; **no** verificado que funcione. Sigue abierto qué
controlador tiene la placa (el fabricante documenta CST9217, el proyecto arrastra
dependencia FT5x06 y el código usa TouchDrvCST92xx).

### Filas de buffer: por qué la tabla estática no muestra el ahorro

| Filas | Bytes por buffer | Dos buffers | Δ contra 116 filas |
| --- | --- | --- | --- |
| 116 (línea base) | 108 112 B | 216 224 B | — |
| 64 | 59 648 B | 119 296 B | −96 928 B |
| 48 | 44 736 B | 89 472 B | −126 752 B |
| 32 | 29 824 B | 59 648 B | **−156 576 B (152,91 KiB)** |

Los buffers se piden con `heap_caps_malloc(..., MALLOC_CAP_DMA)` en tiempo de
ejecución, así que **cambiar las filas no mueve ni un byte del informe de
`idf.py size`**: compilado con 32 filas el binario mide 1 671 264 B, 16 bytes
menos que con 116, y DIRAM es idéntico. El ahorro de 152,91 KiB es RAM interna
libre en ejecución, y sólo se demuestra leyendo el heap en la placa.

Esa cifra sigue siendo aritmética del tamaño del buffer, exactamente como dice el
plan: no es un ahorro medido, y no dice nada sobre si la UI sigue fluida con más
transferencias por refresco.

## 3. Lo que la instrumentación va a medir

`res_metrics` acumula siempre y imprime como máximo una vez cada
`CONFIG_RES_METRICS_LOG_PERIOD_MS` (30 s por defecto). Sin logs por evento ni por
píxel.

| Canal | Qué mide | Meta del plan |
| --- | --- | --- |
| `lcd_flush` | Del `draw_bitmap` al callback de DMA lista | — |
| `lvgl_tick` | `lv_timer_handler()` + `ui_tick()` | 30 FPS durante animaciones |
| `uart_drain` | Intervalo entre drenajes de la UART | p99 <25 ms a 115200 baud |
| `map_match` | Duración de una consulta de mapa | p95 <200 ms a 1 Hz |
| `fix2disp` | De fix válido a flush completado | p95 <200 ms |

Los canales `uart_drain`, `map_match` y `fix2disp` ya existen pero todavía no
tienen quién los alimente: se conectan en P02 y P04, donde están sus dueños. En
P01 sólo se instrumentaron el flush y el tick de LVGL, que es lo que vive en este
componente.

Memoria: libre, mayor bloque y mínimo histórico de heap interno, capaz de DMA y
PSRAM. Stacks: marca de agua actual y mínima vista por tarea registrada (por
ahora la de LVGL). Errores: contadores por categoría, incluido
`RES_ERR_ALLOC_FAILED`, que ahora registra cuánto se pidió y cuál era el mayor
bloque libre antes de que el `assert` corte el arranque.

### Percentiles que no mienten

El histograma tiene cubetas fijas, así que `res_stat_percentile_us()` devuelve el
**borde superior** de la cubeta donde cae el percentil: un p95 de 2 000 µs
significa «el 95 % estuvo por debajo de 2 000 µs». El máximo se guarda aparte y
es exacto. Si el percentil cae en la última cubeta, la función devuelve
`UINT32_MAX` y el log escribe `p95=>64000us` en lugar de inventar una cifra.

Probado en host: `host_tests/test_res_stats.c`, 11 casos, incluidos el borde
exacto de cubeta, el percentil que cae en la última cubeta, la estructura estática
sin `reset` previo y el desborde del contador de milisegundos del limitador de
logs.

## 4. Verificación ejecutada

| Qué | Resultado |
| --- | --- |
| Build configuración por defecto | Compila; tabla de la sección 2 |
| Build con `CONFIG_BOARD_ENABLE_TOUCH=y` | Compila |
| Build con `CONFIG_BOARD_LVGL_BUF_ROWS_32=y` | Compila; `CONFIG_BOARD_LVGL_BUF_ROWS=32` llega al build |
| `make -C host_tests test` | `tile_reader baseline: 13 fixes match the golden`, `res_stats tests passed` |
| `make -C ../starlink_pilot/host_tests test` | En verde |

La caracterización del matcher de P00 sigue dando el mismo golden, lo que confirma
que este cambio no tocó el camino de mapas.

## 5. Lo que falta para cerrar P01

Ninguno de estos puntos se puede hacer sin la unidad; todos son requisito de la
puerta de aceptación del plan:

1. **Tabla de mediciones antes/después de heap real**: arrancar con 116 filas,
   registrar el resumen de `res_metrics` con perfil combinado, repetir con 64, 48
   y 32 filas. Comparar heap interno libre, mayor bloque y mínimo histórico.
2. **Colores y áreas correctos** en cada configuración de filas: fotos de la
   pantalla, no sólo «compila». Con menos filas hay más transferencias por
   refresco y el `rounder_cb` redondea a pares; hay que confirmar que no aparecen
   costuras ni desgarros.
3. **Fluidez**: comparar `lvgl_tick` y `lcd_flush` con la misma UI y el mismo
   replay entre configuraciones. Si 32 filas empeora la fluidez, el ahorro de
   152,91 KiB no es gratis y hay que decidirlo con los dos números a la vista.
4. **Ausencia de fugas al apagar/encender**: ciclos de encendido comparando el
   mínimo histórico de heap.
5. **Periféricos de a uno**: touch primero, verificando que su ausencia o un
   error de bus no impidan mostrar velocidad. IMU y audio quedan apagados.
6. **Identidad de la placa**: `board_log_identity()` imprime en el arranque lo
   declarado por configuración junto a lo que se lee del chip (cores, revisión,
   tamaño de flash, PSRAM detectada). Ese log es el que resuelve varios pendientes
   de la sección 7 de P00 la primera vez que se flashee.
7. **Variante -G y GPIO18**: sin resolver. Decide si el UART del Ruptela puede
   quedarse donde está.

El experimento de staging en PSRAM que menciona el plan sigue siendo posterior y
no es requisito para cerrar P01.
