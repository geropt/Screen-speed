# Waveshare 1.75: hardware, RAM, pantalla y presupuesto de recursos

Investigación: 2026-09-04/05. Documento técnico de apoyo al
[blueprint](blueprint-firmware.md). No se cambiaron drivers ni configuración.
Los valores calculados no sustituyen mediciones de heap, consumo o latencia en
la unidad física. KiB = 1024 bytes; MiB = 1024 KiB.

Lectura rápida: [placa](#1-placa-objetivo-y-discrepancias-que-hay-que-cerrar),
[pines y periféricos](#2-asignación-de-recursos-de-placa),
[RAM y GRAM](#3-qué-significa-ram-y-qué-significa-vram-aquí),
[buffers](#4-coste-exacto-de-los-buffers-actuales),
[build](#5-foto-estática-del-build-disponible),
[presupuestos](#6-política-de-asignación-y-presupuestos-iniciales)
y [mediciones](#7-cpu-sd-y-medición-que-decide-el-diseño).

## 1. Placa objetivo y discrepancias que hay que cerrar

La referencia aportada es **ESP32-S3-Touch-AMOLED-1.75**. Waveshare especifica
ESP32-S3R8, dos núcleos hasta 240 MHz, 512 KiB SRAM, 8 MiB PSRAM y flash de
16 MiB. El panel es 466 × 466, QSPI, controlador CO5300; touch CST9217.
Wi-Fi es de 2,4 GHz y Bluetooth es LE; no diseñar alrededor de Wi-Fi de 5 GHz
o Bluetooth clásico. Fuentes: [documentación Waveshare](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75)
y [repositorio oficial](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75).

| Comparación | Evidencia local | Decisión propuesta |
| --- | --- | --- |
| Controlador de panel | `esp_lcd_sh8601` y macros SH8601 | Confirmar revisión física y secuencia CO5300; no reemplazar el driver solo por el nombre |
| Capacidad flash | `sdkconfig`: 16 MB; `sdkconfig.defaults`: 8 MB | Unificar perfil reproducible de esta placa en 16 MB, después de comprobar unidades |
| PSRAM | Octal a 80 MHz, tamaño autodetectado | Registrar tamaño físico y memoria utilizable al arrancar |
| Touch | `USE_TOUCH=0` | Incorporarlo con un único propietario del bus I2C y eventos a UI |
| SDK y UI | Build con IDF 5.4.4; LVGL 8.4; proyecto EEZ declara 8.3 | Congelar primero la combinación funcional; probar regeneración y upgrades por separado |
| SD | SDMMC de 1 bit, código ajusta reloj hasta 40 MHz | Medir 20/40 MHz y errores con tarjetas reales; no suponer modo 4 bits disponible |

El BSP oficial publicado actualmente pertenece a otra línea de dependencias:
su inventario lista versión 3.0.1 con IDF ≥ 5.5, y el header usa la API de display
LVGL 9. Es referencia para pines/controladores, no un reemplazo directo para
este proyecto LVGL 8. Fuentes: [inventario oficial de componentes](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/docs/components.md)
y [header BSP](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/master/bsp/esp32_s3_touch_amoled_1_75/include/bsp/esp32_s3_touch_amoled_1_75.h).

Antes de aprobar hardware: foto/SKU/revisión, esquema correspondiente, flash ID,
tamaño PSRAM detectado, panel, alimentación, conversor serie y tarjeta. La variante
`-G` incorpora GNSS LC76G: hay que comprobar sus conexiones antes de utilizar
GPIO18 para el Ruptela. No habilitar un segundo GPS sin resolver esta posible
colisión y definir cuál es la fuente de posición.

## 2. Asignación de recursos de placa

Resumen de conexiones del fabricante; verificar contra la revisión instalada.
Fuente de esta tabla: [referencia de hardware Waveshare](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/blob/main/HARDWARE_REFERENCE.md).

| Recurso | GPIO / conexión |
| --- | --- |
| AMOLED QSPI | CLK 38, CS 12, D0–D3: 4/5/6/7, RESET 39, TE 13 |
| Touch | INT 11, RESET 40 |
| I2C compartido | SCL 14, SDA 15 |
| SDMMC 1 bit | CMD 1, CLK 2, D0 3; D3/CS 41 |
| USB nativo | D− 19, D+ 20 |
| UART0 / consola | TX 43, RX 44 |
| Expansión | GPIO16/17/18; 17/18 requieren revisar GNSS en variante -G |
| Audio I2S | MCLK 42, BCLK 9, WS 45, DOUT 8, DIN 10, amplificador 46 |
| IMU | INT2 21 |
| BOOT | GPIO0, también strap de arranque |
| Expansor TCA9554 | P3 RTC IRQ, P4 botón PWR, P5 AXP IRQ, P6 IMU IRQ, P7 GPS reset |

No asignar nuevos periféricos a pines aparentemente libres del ESP32 sin revisar
la PCB. El botón PWR no es otro GPIO directo. Mantener USB y BOOT como camino de
recuperación durante desarrollo. El driver actual no utiliza TE ni detecta
extracción de SD mediante un pin dedicado.

### Periféricos disponibles: utilidad y límites

La placa incluye AXP2101 para energía/batería, QMI8658 de seis ejes, PCF85063 RTC,
ES8311 para salida de audio, ES7210 para adquisición de micrófonos y TCA9554.
Fuentes: [producto Waveshare](https://www.waveshare.com/product/esp32-s3-touch-amoled-1.75.htm)
y [recursos y esquema](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75/Resources-And-Documents).

Aplicación propuesta, no funcionalidad existente:

- AXP2101: estado de alimentación/batería, autorización de mantenimiento y cierre
  ordenado. Verificar qué mediciones ofrece realmente el montaje y su calibración.
- RTC: conservar una referencia de hora tras reinicios. No tratar cualquier hora
  leída como confiable para TLS; mantener origen y validez.
- IMU: orientación de pantalla y señal complementaria de movimiento. No reemplaza
  GPS ni demuestra por sí sola que un vehículo está detenido.
- Touch: navegación de servicio, emparejamiento, diagnóstico y brillo. Los
  callbacks publican acciones; no ejecutan descargas ni consultas SD.
- Audio: avisos breves opcionales y configurables; primero medir consumo y
  convivencia I2S/DMA. No habilitar micrófonos ni reconocimiento de voz por defecto.
- USB: logs, recuperación y soporte. No exportar la SD como almacenamiento USB
  mientras FAT esté montado simultáneamente en el firmware.

Concentrar inicialización y acceso I2C en `board_services`. El puerto actual usa
el driver I2C legacy: migrar conjuntamente sus consumidores si se adopta el nuevo
driver, sin dos inicializaciones rivales del mismo controlador. IRQ solo notifica;
la transacción I2C se hace fuera de la interrupción.

La entrada de alimentación de la Waveshare no debe confundirse con una entrada
automotriz directa de 12/24 V. Documentar regulación, protección, masas, caída de
tensión al arrancar el vehículo y conversión RS232 del montaje antes de liberar.
No fijar corrientes máximas, autonomía ni temperaturas admisibles sin esquema,
componentes y medición del producto terminado.

## 3. Qué significa RAM y qué significa «VRAM» aquí

| Memoria | Uso en el producto | No confundir con |
| --- | --- | --- |
| SRAM interna del ESP32-S3 | Código/datos internos, stacks, DMA y parte del sistema/red | 512 KiB íntegramente libres para la aplicación |
| PSRAM externa | Caché de mapas, objetos/assets y buffers grandes compatibles | RAM interna de igual latencia o válida para todo DMA |
| Flash externa | Firmware, assets persistentes, NVS, particiones | RAM para ejecutar asignaciones dinámicas |
| GRAM dentro del CO5300 | Imagen que conserva el controlador de pantalla | Heap del ESP32 o memoria utilizable por mapas/Wi-Fi |

El chip también dispone de 16 KiB de SRAM RTC y 384 KiB de ROM. La ROM no se
asigna como heap; la SRAM RTC es candidata a estado pequeño retenido según modo
de energía, no a resolver buffers de pantalla. No confundirla con el chip RTC
PCF85063 de la placa. Fuente: [datasheet ESP32-S3, CPU y memoria](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf).

No hay una GPU con VRAM general que pueda repartirse entre tareas. El CO5300
declara 1.843.200 bits de GRAM, equivalentes a **230.400 bytes / 225 KiB**, con
almacenamiento interno comprimido. Permite actualizar ventanas; esa memoria
no es direccionable como RAM de la CPU y no corresponde a un framebuffer RGB565
lineal disponible para el programa. Fuente: [datasheet CO5300, descripción y características, páginas 6–7](https://dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf).

Consecuencia de diseño: dejar que el panel conserve los píxeles ya enviados y
renderizar solamente las áreas que cambian. El HUD no necesita dos imágenes
completas en SRAM para mostrar velocidad, límite e indicadores.

## 4. Coste exacto de los buffers actuales

La configuración local usa RGB565 de 2 bytes por píxel. En
[el puerto LVGL](../main/waveshare_amoled_lcd_port.cpp) se asignan dos buffers
`MALLOC_CAP_DMA` de `466 × (466 / 4)` píxeles; la división entera produce 116 filas.

| Organización RGB565 | Bytes | KiB | Diferencia frente al actual |
| --- | ---: | ---: | ---: |
| Actual: 2 × 466 × 116 × 2 | 216.224 | 211,16 | — |
| Candidato: 2 × 466 × 64 × 2 | 119.296 | 116,50 | −94,66 KiB |
| Candidato: 2 × 466 × 48 × 2 | 89.472 | 87,38 | −123,78 KiB |
| Candidato inicial: 2 × 466 × 32 × 2 | 59.648 | 58,25 | −152,91 KiB |
| Una imagen completa: 466 × 466 × 2 | 434.312 | 424,13 | No propuesto para SRAM |
| Dos imágenes completas | 868.624 | 848,27 | No caben en SRAM interna |

Estos números cuentan píxeles, no descriptores SPI, objetos LVGL, stacks ni
buffers temporales del driver. La recuperación potencial de 152,91 KiB con 32
filas es aritmética exacta, pero su rendimiento aún debe probarse.

LVGL 8 admite renderizado parcial y dos buffers con transferencia asíncrona;
`flush_ready` libera el buffer después de finalizar el envío. No es obligatorio
dimensionarlos a una pantalla completa. Fuente: [interfaz de display LVGL 8.4](https://lvgl.io/docs/open/8.4/porting/display).

### Estrategia A: primera optimización recomendada

1. Mantener RGB565, driver funcional y dos buffers internos DMA.
2. Parametrizar 32/48/64/116 filas, empezando el benchmark por 32. Conservar
   restricciones de alineación y redondeo de áreas del puerto.
3. No tocar la matemática del matcher ni actualizar LVGL en este cambio.
4. Medir pantalla normal, cambio de todos los dígitos, splash, arco de alerta,
   transición de pantalla y progreso de instalación.
5. Elegir el menor tamaño que cumpla latencia, ausencia de corrupción y margen
   interno bajo Wi-Fi/TLS. Si 32 penaliza demasiado CPU/transacciones, probar 48.

`trans_queue_depth=10` no representa diez buffers completos reservados, pero
permite transacciones en vuelo que deben tener buffers con vida útil válida.
Limitar conscientemente esa concurrencia y observar asignaciones temporales.

### Estrategia B: renderizado en PSRAM, solo si aporta una mejora medida

Es posible diseñar buffers grandes de renderizado en PSRAM y uno o dos buffers
internos pequeños de transferencia. El worker copia franjas, las envía y no
notifica a LVGL que terminó hasta liberar correctamente el origen completo.
Esto exige una máquina de estados de flush y no solamente cambiar flags de malloc.

En el SPI master de IDF 5.4.4 inspeccionado, `setup_priv_desc()` comprueba si el
buffer sirve para DMA; de no servir, puede asignar otro buffer DMA y copiarlo.
Una imagen grande en PSRAM puede por tanto disparar una asignación interna grande
oculta. Fuente: [implementación SPI master fijada a v5.4.4](https://github.com/espressif/esp-idf/blob/v5.4.4/components/esp_driver_spi/src/gpspi/spi_master.c).

No afirmar que el S3 nunca puede hacer DMA desde PSRAM: la capacidad del hardware
y el camino efectivo de este driver son cuestiones diferentes. La aceptación
de B exige probar el driver/versionado concretos y demostrar un máximo acotado
de RAM interna, buffers no reutilizados prematuramente y recuperación de errores.
No activar `full_refresh` ni doble framebuffer por costumbre.

### Límite de transporte del panel

El [driver local](../components/esp_lcd_sh8601/include/esp_lcd_sh8601.h) configura
QSPI a 40 MHz. Cálculo ideal: `40 MHz × 4 bits / 8 = 20 MB/s`. Una pantalla
RGB565 requiere al menos 21,72 ms de datos: aproximadamente 46 imágenes/s como
techo puramente teórico, antes de comandos, pausas, copias y renderizado.
A 30 pantallas completas/s serían 13,03 MB/s. No es un FPS medido ni garantizado.

Objetivo propuesto: buena legibilidad y reacción al dato; 30 FPS durante
animaciones si el presupuesto lo permite, con áreas sucias pequeñas. Explorar TE
después de confirmar panel y driver; no esperar que TE acelere el bus. El brillo
se gestiona mediante el controlador AMOLED, no con un GPIO de backlight que
el puerto actual define como `-1`. Limitar brillo sostenido y elementos estáticos
prolongados como política de producto, validada con el panel real.

## 5. Foto estática del build disponible

Se ejecutó `esp_idf_size` sobre `build/Offline_maps.map` existente durante esta
investigación, sin recompilar ni medir hardware:

| Informe del linker | Bytes |
| --- | ---: |
| Used static IRAM, región informada | 16.383 |
| Used stat D/IRAM | 115.407 |
| Flash `.text` | 535.292 |
| Flash `.rodata` | 1.040.068 |
| Total image size sin padding final | 1.688.886 |

El informe deja 1 byte en una región IRAM de 16 KiB y 226.353 bytes en su región
D/IRAM. **No significa que quede un byte de RAM para ejecutar el producto**, ni
que 226.353 sea el heap libre de runtime. Son regiones/secciones del enlace:
arranque, cachés, reservas, heaps y asignaciones cambian la disponibilidad real.
No sumar aliases IRAM/DRAM como si fueran dos memorias físicas independientes.

El `sdkconfig` habilita `CONFIG_SPIRAM_RODATA`: hay que presupuestar también
la copia de rodata en PSRAM. El orden de magnitud observado es 1 MiB, además de
los 2 MiB de caché de mapas. Los assets grandes ya tienen coste de memoria;
«hay 8 MiB» no equivale a «puedo dar 8 MiB al mapa».

La caché actual cuenta bytes de payload contra los 2 MiB; su índice de 512
entradas se asigna aparte en PSRAM, aproximadamente 12 KiB con el ABI de 32 bits
del target. Además, asigna el tile entrante antes de desalojar: hay un pico
transitorio que el límite nominal no evita. El presupuesto nuevo debe cubrir
también ese índice y controlar el orden de asignación/expulsión.

## 6. Política de asignación y presupuestos iniciales

ESP-IDF distingue capacidades de memoria. El umbral de `malloc` indica
preferencia con posible fallback, no una garantía de ubicación. La reserva
interna evita que asignaciones generales consuman todo lo requerido por DMA;
no añade memoria. Stacks siguen normalmente internos; PSRAM puede quedar
inaccesible durante operaciones con caché deshabilitada. La configuración de
instrucciones/rodata externas cambia ese comportamiento y debe validarse como
conjunto. Fuente: [RAM externa, IDF 5.4.4 ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-guides/external-ram.html).

Reglas propuestas para nuestros componentes:

- Asignaciones grandes explícitas por capacidad; no depender accidentalmente
  de que `malloc` elija PSRAM o RAM interna ese día.
- Memoria interna: ring UART, estado crítico, colas/notificaciones, stacks
  críticos y buffers/descriptores de periféricos que lo requieran.
- PSRAM: caché de tiles/índices, assets y objetos no críticos compatibles,
  pools de records y bloques de transferencia cuando su consumidor lo soporte.
- Buffers de tamaño fijo/pools en caminos repetitivos. Evitar malloc/free por
  byte UART, fix GPS, frame UI o record de telemetría.
- Fallar con error recuperable al agotar una función opcional. No reiniciar el
  HUD por no poder abrir un segundo cliente TLS o una sesión de mapas.

Presupuesto PSRAM de partida, **no medición ni promesa de cabida**:

| Reserva funcional | Presupuesto inicial | Observación |
| --- | ---: | --- |
| RoData trasladada | ≈1 MiB | Revisar por build; no sumar assets otra vez si ya están aquí |
| Caché de mapas total | 2 MiB | Presupuesto nuevo incluyendo índice/metadatos; hoy los 2 MiB cuentan solo payload |
| UI dinámica/decodificación | 512 KiB | No incluye buffers DMA internos; assets rodata no se repiten |
| Pools de records + bloques de instalador | 256 KiB | Topes combinados, no cargar el paquete completo |
| Red/TLS aptos para PSRAM | 512 KiB | Reserva de diseño; mover solo lo soportado y medido |
| Margen PSRAM sin comprometer | ≥1 MiB | Protege picos y fragmentación |

La suma orientativa es 5,25 MiB; lo restante no se adjudica todavía. Si una
configuración consume más rodata/instrucciones, se vuelve a calcular. Después de
medir red/UI se puede ensayar caché de 3–4 MiB contra hit rate y latencia; no
maximizarla a costa del mantenimiento.

Presupuesto interno: reservar primero 58,25 KiB de píxeles para la alternativa A,
8 KiB de ring UART como candidato, stacks según medición y los pools críticos.
El consumo de Wi-Fi/BLE/TLS, drivers y sistema se obtiene por diferencias entre
perfiles; no inventar una cifra de «RAM restante» a partir del linker. Proponer
≥64 KiB de heap interno libre mínimo en el perfil combinado como primera meta
de ingeniería, revisable con evidencia; controlar además el mayor bloque libre.

### Configuraciones a experimentar, una por vez

| Configuración local | Acción propuesta y condición |
| --- | --- |
| `SPIRAM_MALLOC_ALWAYSINTERNAL=16384` | Medir impacto; usar capacidades explícitas para buffers propios |
| `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` | Evaluar 48/64 KiB después de reducir display; mayor reserva también restringe malloc general |
| `SPIRAM_RODATA=y`; fetch instrucciones/XIP desactivados | Medir copia/caché; evaluar XIP como experimento separado por impacto flash/OTA/PSRAM |
| `MBEDTLS_INTERNAL_MEM_ALLOC=y` | Medir handshake; evaluar asignación externa soportada, manteniendo márgenes y pruebas de caché |
| Preferencia Wi-Fi/lwIP por PSRAM desactivada | Comparar activada con misma carga, si la opción es válida en el SDK fijado |
| Wi-Fi RX estático 10; dinámicos RX/TX 32/32 | Ajustar contra throughput y retransmisión; no reducir todos a la vez |
| Wi-Fi IRAM optimizations activas | Medir tradeoff código interno/rendimiento; no asumir ahorro libre de efectos |
| Caché instrucciones 16 KiB, datos 32 KiB, línea 64 B | Benchmark matching/UI/red antes de ampliar; más caché también usa recursos internos |
| `UART_ISR_IN_IRAM` desactivado | Evaluar para mantenimiento con flash; verificar toda la ruta ISR/datos y pérdidas reales |
| `LV_MEM_CUSTOM` con malloc estándar | Instrumentar dónde quedan objetos; allocator propio solo si aporta control comprobable |

Para OTA HTTPS, Espressif documenta descargas HTTP parciales que permiten reducir
el buffer RX TLS de 16 a 4 KiB en ese cliente. Es un ahorro potencial de 12 KiB,
no permiso para reducir globalmente TLS y suponer compatibles todos los servidores.
Fuente: [ESP HTTPS OTA](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/system/esp_https_ota.html).

## 7. CPU, SD y medición que decide el diseño

No usar los dos núcleos como sustituto de colas y ownership: siguen compartiendo
RAM, caché y periféricos. El S3 acelera float simple, no double por FPU; ESP-IDF
puede fijar automáticamente a un núcleo una tarea que usa float. Es preferible
declarar la afinidad del matcher después de medir. Los tamaños de stack de las
APIs de tareas IDF se expresan en bytes. Fuente: [FreeRTOS IDF](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/system/freertos_idf.html).

Coordenadas E7 enteras en la frontera del parser evitan perder precisión antes
de convertir; geometría local y uso de float/double son un cambio de algoritmo
posterior a la extracción de matching. No introducir una librería vectorial o
ensamblador sin perfil que identifique un cuello de botella.

La SD está en un bus distinto del QSPI de pantalla, pero compite por CPU/RAM y
tiempo del sistema. Limitar escrituras/hash a bloques; liberar entre operaciones,
priorizar consultas y no depender de cancelación instantánea de un `fread` ya
iniciado. Mantener timeouts del driver y velocidad visible independiente de SD.
El límite actual de cinco archivos abiertos debe revisarse con mapa, instalador,
logs y journal; ampliar descriptores también consume memoria.

Instrumentación mínima mediante las APIs de
[heap ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-reference/system/mem_alloc.html):

- `heap_caps_get_free_size`, `heap_caps_get_minimum_free_size` y
  `heap_caps_get_largest_free_block` para INTERNAL/8BIT, DMA y SPIRAM.
- High-water de cada stack, máximos de colas y pools, errores de malloc y overflow.
- Latencias UART→snapshot→flush completado; matching frío/caliente y lectura SD.
- Bytes/rectángulos enviados al panel, tiempo de render y de DMA, no solo FPS.
- Consumo eléctrico medido en placa con panel oscuro/brillante, radio y SD.

Perfiles obligatorios: arranque; HUD solo; caché llena; Wi-Fi conectado; handshake
TLS; pairing BLE; upload de mapa; validación de mapa; OTA flash; GPS a máxima
cadencia durante cada uno. Registrar mínimo y pico, no solo un log de memoria al
boot. Ejecutar soak y ciclos repetidos de abrir/cerrar servicios para detectar
fragmentación/fugas. Los criterios de cierre están en el [plan](plan-evolucion.md).
