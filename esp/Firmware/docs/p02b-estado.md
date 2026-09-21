# P02b — Estado del vehículo y arranque degradado

Revisión: 2026-09-05. Segunda mitad de P02 del [plan](plan-evolucion.md), sobre el
framing ya compartido de [P02a](p02a-framing.md).

## 1. Tres errores de la línea base

Encontrados leyendo el código, no supuestos:

| Dónde | Qué pasaba |
| --- | --- |
| `gps_event_handler` en `offline_maps.c` | Llamaba a **`xQueueSendFromISR` desde una tarea**, no desde una ISR, descartando el parámetro de cambio de contexto |
| `app_main` | `err = sd_card_init(); if (err != ESP_OK) return;` — **sin tarjeta no había UART, ni estado, ni velocidad**, con el splash congelado |
| `set_street_name` en `dynamic.c` | Llamaba a `lv_label_set_text` **sin tomar `lvgl_lock`**, desde la tarea principal, mientras la tarea de LVGL corría `lv_timer_handler()` |

El tercero es el más silencioso: el resto de `dynamic.c` sí tomaba el mutex, así
que era una excepción fácil de pasar por alto y produce corrupción sólo cuando las
dos tareas coinciden.

## 2. `components/vehicle_state`

C puro, sin ESP-IDF, sin memoria dinámica, sin locks. El dueño del struct es una
sola tarea y los productores le mandan mensajes acotados.

### Cada señal tiene su propia edad

Es la regla central y el plan la nombra: **«no renovar 418 porque llegó otro
record»**. Que llegue un fix no rejuvenece el estado de GPRS; que llegue un record
cualquiera no rejuvenece la ignición. La API lo hace estructural: sólo
`VEHICLE_MSG_GPRS` toca la marca de tiempo de GPRS.

Hay una prueba que lo fija: con 418 recibido en t=1000 y después 38 fixes más 38
records de ignición, la edad de GPRS sigue siendo 39 000 ms y su estado vencido,
mientras fix e ignición están frescos.

### Tres estados, no un booleano

| Estado | Significado |
| --- | --- |
| `VS_UNKNOWN` | Nunca llegó |
| `VS_FRESH` | Llegó y está dentro de su TTL |
| `VS_EXPIRED` | Llegó, pero hace demasiado |

«Desconocido» y «vencido» son fallas distintas y la UI tiene que poder
distinguirlas. Con un solo booleano de validez no se puede, y era parte del
problema original. El último valor conocido sigue disponible en el snapshot,
declarado vencido: quién lo muestra y cómo es decisión de P03.

TTL iniciales: 3 s para el fix —a 1 Hz son tres fixes perdidos, y el plan fija
«probar 3 s iniciales a 1 Hz»— y 30 s para las señales de IO, que llegan mucho más
espaciadas. Son valores para negociar con mediciones reales.

### `tracker_epoch`

Sube cuando cambia el IMEI, y el estado anterior se invalida: la posición del
vehículo anterior es peor que no tener posición. Conocer el **primer** IMEI no
cuenta como cambio, porque tirar el fix que ya se tenía no aportaría nada.

La época sirve además para rechazar resultados viejos sin depender de tiempos. En
el HUD se usa así: después de un matching que puede tardar leyendo tiles, se
compara la época del snapshot con la actual y, si cambió, el resultado se descarta
en lugar de mostrar el límite de la calle del vehículo anterior.

### El reloj entra por parámetro

Ninguna función lee el reloj: `now_ms` es siempre argumento. Eso hace al componente
determinista y probable en el host, y obliga al llamador a pasar un reloj
monotónico (`esp_timer_get_time()`) en lugar de la hora del GPS, que salta. Un
mensaje con marca anterior a la última aplicada de la misma señal se descarta y se
cuenta en `rejected_stale_clock`.

## 3. Mailbox de fixes contra cola de señales

| Camino | Forma | Por qué |
| --- | --- | --- |
| Fixes | Mailbox de **1 lugar**, `xQueueOverwrite` | Un fix es estado: el último vale y los intermedios no interesan |
| IO e identidad | Cola de **8 lugares**, sin sobrescritura | Un cambio de ignición o de IMEI perdido cambia el significado del resto del estado |

Antes era una sola cola de 5 `gps_t` completos. Con un consumidor lento —tarjeta
trabada, barrido de 9 tiles— acumulaba fixes vencidos y después los procesaba en
ráfaga como si fueran actuales, gastando lecturas de SD en posiciones por las que
el vehículo ya había pasado.

El **outbox de telemetría es otra cosa** y va aparte: ahí ningún dato se puede
perder por sobrescritura. Se define en P05.

## 4. Arranque sin tarjeta y recuperación

- El montaje ya no condiciona el arranque. Sin tarjeta se muestra velocidad y no
  se muestra límite de mapa.
- `sd_watch` reintenta cada 5 s y avisa una sola vez, no en cada intento.
- Al montar, **sólo marca disponibilidad**: no hay reset global ni se reinicia el
  enlace. La generación de mapas y la barrera de lectores son de P04/P07.
- Con la tarjeta ausente no se intenta el matching, lo que evita 9 aperturas
  fallidas por fix.

## 5. Callbacks acotados

Regla de la etapa: ningún callback del enlace toca UI, SD, DNS ni espera ACK.

- Los tres puentes (`on_ignition`, `on_gprs`, `on_imei`) y el handler de GPS sólo
  arman un `vehicle_msg_t` de ~40 B y vuelven. Antes se copiaba el `gps_t`
  completo.
- El log por fix bajó de INFO con 11 argumentos a DEBUG: a 1 Hz era tráfico
  constante emitido **desde la tarea que drena la UART**.
- Si la cola de señales está llena se cuenta en `RES_ERR_QUEUE_FULL` y se
  descarta, en lugar de bloquear el drenaje.
- El consumidor espera 250 ms en lugar de `portMAX_DELAY`, porque el estado
  envejece aunque no llegue nada y «vencido» debe poder mostrarse sin esperar el
  próximo fix.

## 6. Verificación ejecutada

| Qué | Resultado |
| --- | --- |
| `test_vehicle_state` | 16 casos con ASan/UBSan, en verde |
| Build del HUD | Compila. 1 671 440 → 1 675 344 B (+3 904 B), DIRAM +24 B, IRAM sin cambio (16 383 de 16 384) |
| Las 6 suites de host | En verde; el golden del matcher no se movió |

## 7. Lo que sigue abierto

- **Nada se flasheó.** Que el arranque sin tarjeta muestre velocidad y que el
  remount no interrumpa el enlace necesitan la placa y una tarjeta que se pueda
  extraer en caliente. Igual las señales de IO reales necesitan banco con IMEI
  propio.
- El `speed_limit = 78` inicial de `vars.c` sigue ahí: es el «límite inicial
  ficticio» que el plan manda retirar en **P03**, junto con mostrar
  desconocido/vencido/inferido/último conocido.
- La UI todavía se escribe desde la tarea principal. El escritor único de widgets
  es P03; acá sólo se corrigió la escritura sin mutex.
- El parseo de campos NMEA sigue acoplado a `esp_gps_t` y con doble verificación
  de checksum, heredada de P02a.
- El comportamiento ante **ignición desconocida** y el modo servicio no están
  definidos: el estado ya distingue desconocido de vencido, pero qué hace el
  producto con eso es P03.
