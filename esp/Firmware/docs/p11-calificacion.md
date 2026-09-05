# P11 — Calificación integrada: qué está verificado y qué no

Revisión: 2026-09-05. Cierra la ejecución del
[plan de evolución](plan-evolucion.md) hasta donde se puede llegar **sin hardware**.

**P11 no está hecha.** El paquete P11 del plan es soak de 24/72 h, campo controlado,
pruebas eléctricas y térmicas, y una matriz combinada sobre la Waveshare final. Nada de
eso ocurrió. Este documento es el informe de límites que el plan pide como parte de la
calificación, no la calificación.

## 1. Corrección al alcance previsto

Cuando se planificó esta etapa se asumió que no se podía compilar el firmware. **Era
incorrecto**: ESP-IDF 5.4.4 está instalado y el HUD y el piloto compilan. Lo que falla
en esta máquina es `export.sh`; el camino que funciona es
`activate_idf_v5.4.4.sh`, y quedó documentado en [P00](baseline-P00.md).

Así que sí hubo build en cada etapa. Lo que sigue sin ocurrir es **flashear**.

## 2. Lo ejecutado

Un solo comando corre todo lo verificable sin hardware:

```sh
./tools/run_tests.sh              # host: HUD, piloto, herramientas
./tools/run_tests.sh --with-build # además compila el firmware
```

| Suite | Casos | Qué fija |
| --- | --- | --- |
| `test_map_match_equivalence` | 5 | **Equivalencia v1**: el matcher extraído reproduce el golden de P00 |
| `test_map_match_robustness` | 10 | Truncamiento en cada offset, encabezados que mienten, barrido de 9 tiles |
| `test_capsule_equivalence` | 4 | **Equivalencia de cápsula**: leyendo por rangos, mismo golden |
| `test_map_installer` | 20 | Inyección de fallas en cada transición persistente |
| `test_ruptela_framing` | 24 | Framing binario-seguro: corte en cada offset, NUL/newline/`$`, overflow |
| `test_ruptela_vectors` | 8 | Vectores independientes del protocolo, incluido cmd68 |
| `test_vehicle_state` | 16 | Edad por señal, `tracker_epoch`, GPS vencido |
| `test_ui_model` | 23 | Estados de límite y velocidad, histéresis de la alerta |
| `test_outbox` | 15 | Pendiente/en vuelo/confirmado con servidor simulado |
| `test_backup_policy` | 14 | Permiso, dwell, revocación |
| `test_pairing` | 15 | Posesión única, ventana, sesión atada al bond |
| `test_ota_policy` | 19 | Verificación, autoprueba acotada, anti-rollback |
| `test_res_stats` | 11 | Percentiles como cota superior declarada |
| Piloto: `ruptela_io_parser`, `ruptela_proto` | 12 | Records por longitud, CRC, ACK |
| **Total** | **196** | Todos con ASan y UBSan |

Las **dos puertas de aceptación** de las extracciones son los dos replays de
equivalencia. Ambos comparan contra el mismo golden congelado en P00, generado antes de
tocar nada: si uno se rompe, el matcher o el contenedor cambiaron resultados.

## 3. Cobertura contra la matriz mínima de fallas del plan

Honestidad primero: **cubierto** significa que hay una prueba automatizada que falla si
el comportamiento se rompe. **Parcial** significa que hay lógica y prueba, pero el caso
real necesita hardware o un banco. **No cubierto** significa que no hay prueba.

| Área | Casos obligatorios | Estado |
| --- | --- | --- |
| **Framing** | Binario con NUL/newline/`$`, mensajes concatenados, corte en cada offset, CRC malo, ruido largo, overflow | **Cubierto** (24 casos). Relock de baud: no cubierto, necesita el enlace |
| **Estado** | RMC inválido, campos ausentes, UTC que salta, GPS silencioso, IO ausente, IMEI que cambia | **Cubierto** para edad, vencimiento, IMEI y publicación sólo tras validación. UTC que salta: mitigado por diseño —el reloj es monotónico— pero sin prueba propia |
| **UI** | Sin GPS/SD, límite desconocido/inferido/vencido, encendido/apagado durante flush, cambio de pantalla durante alerta, errores de touch | **Parcial**: los estados de límite y velocidad están cubiertos (23 casos). Flush, cambio de pantalla y touch **no**: necesitan la placa |
| **Mapas** | Frío/caliente, bordes, colectoras, vía anónima, reset, truncamiento en cada campo, cambio de generación con lectores | **Parcial**: truncamiento, vía anónima, reset y equivalencia cubiertos. **Barrera de lectores no implementada** |
| **Caché/SD** | OOM, negativo por ausencia vs EIO, fragmentación, tarjeta lenta/extraída, remount, límite de archivos | **No cubierto**. `map_store.io_errors` distingue en el camino de cápsula; en el de directorio sigue en 0 porque `tile_cache_get()` no dice por qué falló |
| **Red** | SSID ausente, contraseña mala, DHCP/DNS lento, socket que no responde, AP+STA, BLE bajo carga | **No cubierto**. `connectivity_manager` envuelve `esp_wifi` y no tiene ninguna prueba |
| **Telemetría** | 418 vuelve/vence en DNS/connect/send/ACK, ACK perdido/duplicado/NACK, cola llena, reinicio, backlog | **Parcial**: ACK/NACK/timeout/cola llena cubiertos con servidor simulado. La revocación en **cada etapa** del envío necesita el enlace |
| **Pairing** | Cliente no autorizado/revocado, repetición de sesión, varios clientes, teléfono nuevo, pérdida de secreto | **Parcial**: la lógica está cubierta (15 casos). Sin BLE, nada de esto se probó sobre un transporte real |
| **Paquetes** | Firma/hash malos, región equivocada, path traversal, overflow de offset, chunks repetidos, disco lleno | **Parcial**: hash, chunks repetidos, offsets y disco lleno cubiertos. **Firma no**: está en cero. Path traversal no aplica porque los paths son internos |
| **Persistencia** | Corte antes/después de cada sync/checkpoint/selector; candidato dañado; RAM distinta de durable | **Parcial**: inyección de fallas en cada transición (20 casos) con almacenamiento simulado. **El corte físico real no está modelado** |
| **OTA** | Imagen para otra placa, slot insuficiente, bootloader sin rollback, B defectuosa, reset antes de confirmar | **Parcial**: las decisiones están cubiertas (19 casos). El camino real **no**, y `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` sigue apagado |
| **Energía** | Fuente débil, brownout, corte con SD/radio, ignición OFF, batería baja | **No cubierto**. Nada de esto se puede simular sin la placa |

Resumen: 2 áreas cubiertas, 7 parciales, 3 sin cubrir. Las tres sin cubrir —caché/SD,
red y energía— son exactamente las que dependen de periféricos físicos.

## 4. Lo que el verde NO significa

Esto está también en la salida del runner, para que nadie lea un verde de CI como
aprobación del producto:

- **No se flasheó ninguna placa.** Memoria real, latencia, colores, fluidez, consumo y
  estabilidad siguen sin medir. Todas las metas numéricas del plan siguen siendo metas.
- **No se probó contra un Ruptela real.** Hace falta banco con IMEI propio; usar el
  tracker operativo está prohibido por el plan.
- **No hay cortes físicos de alimentación.** El almacenamiento simulado prueba máquinas
  de estados, no el comportamiento de la FAT ante un corte: escrituras reordenadas,
  sectores a medio escribir y el caché de la controladora no están modelados.
- **Los tiempos del host no son comparables.** El host es órdenes de magnitud más
  rápido y `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` es un `malloc` común.
- **Integridad no es autenticidad.** Los CRC-32 de la cápsula detectan corrupción, no
  manipulación. La firma está en cero.
- **El workflow de CI no se ejecutó en GitHub Actions.** Parsea y sus comandos son los
  mismos del runner local, pero la primera corrida real puede necesitar ajustes.

## 5. Estado por paquete

| Paquete | Entregado | Cerrado | Qué falta para cerrar |
| --- | --- | --- | --- |
| P00 | Dossier, hashes, build reproducible, arnés | **Casi** | SKU/revisión de placa, variante -G, flash/PSRAM del chip |
| P01 | Servicios de placa, instrumentación, filas configurables | No | Heap real por configuración, fotos, fluidez, ciclos de encendido |
| P02a | Framing compartido binario-seguro | **Casi** | Un stream mixto real capturado del Ruptela |
| P02b | Estado con edad, arranque sin SD | No | Tarjeta extraíble en caliente, señales IO reales |
| P03 | `ui_model`, presenter único escritor | No | Fotos de cada estado, velocidad con SD lenta, traza medida |
| P04 | Matcher extraído con equivalencia probada | **Casi** | Corpus de los seis logs de referencia; barrera de lectores |
| P05 | Outbox, política, vectores, dueño de la radio | No | Servidor de prueba, banco con IMEI, integración en el firmware |
| P06 | — | No iniciado | El plan lo deja diferido a propósito |
| P07 | Cápsula con equivalencia, instalador transaccional | No | Cortes físicos, medición en FAT, transporte |
| P08 | Contratos de pairing y capacidades | No | Transporte BLE, teléfonos reales |
| P09 | Política de OTA A/B | No | Bootloader con rollback habilitado, camino real |
| P10 | Threat model, decisiones de custodia listadas | No | Secure Boot, eFuses, provisión, claves |
| P11 | Runner y CI | No | Soak 24/72 h, campo, eléctrico/térmico |

## 6. Lo primero que conviene hacer cuando haya una placa

En este orden, porque cada paso desbloquea al siguiente:

1. **Flashear y leer el log de arranque.** `board_log_identity()` imprime lo declarado
   contra lo que se lee del chip: cores, revisión, tamaño de flash, PSRAM detectada.
   Eso resuelve de una sola vez varios pendientes de P00 y P01.
2. **Confirmar la variante -G**, que decide si GPIO18 puede seguir siendo el UART del
   Ruptela. Si no puede, hay que mover el pin antes de cualquier medición de enlace.
3. **Leer el resumen de `res_metrics`** con perfil combinado: heap interno, mayor
   bloque, mínimo histórico, y las latencias de flush y tick. Ahí aparecen las primeras
   cifras reales contra las metas del plan.
4. **Repetir con 64, 48 y 32 filas de buffer** para saber si los 152,91 KiB son reales
   y qué cuestan en fluidez.
5. **Habilitar `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`** y verificar que el bootloader
   instalado realmente lo soporte, antes de escribir una línea más de OTA.
6. **Banco con IMEI propio** para todo el camino de telemetría.

## 7. Deuda conocida, en un solo lugar

- Barrera de lectores para cambio de generación con consultas en curso (P04/P07).
- Caché negativa que distinga ausencia de error de E/S en el camino de directorio.
- Política de expulsión bajo OOM en la caché de tiles.
- Agrupación por ciclo de `telematics.c`: lógica no trivial, mucho estado de archivo,
  cero pruebas.
- Journal durable del outbox: hoy vive en RAM y un reinicio pierde lo pendiente.
- Firma del manifiesto de la cápsula y de la imagen de firmware.
- Modo servicio y pantallas de mantenimiento: no se hicieron a propósito, porque
  agregar una pantalla sin función detrás sería simular algo que no existe.
- El parseo de campos NMEA sigue acoplado a `esp_gps_t`, con doble verificación de
  checksum heredada de P02a.
- Retención de ediciones viejas de mapa y cálculo de espacio real.
