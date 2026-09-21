# NMEA GPS Replay — Flipper Zero FAP

Reproduce un log crudo del RS232 del GPS por el **TX del Flipper (pin 13)** hacia la
**ESP32 GPIO18**, 8N1, como si fuera la salida del conversor RS232→TTL del setup real.
Arranca a **9600 baud** (tope del Ruptela Pro5-Lite/HCV5-Lite) y se puede alternar a
**115200** (default de Pro5/HCV5) con **◄/►**, en caliente, para probar el auto-detect
de baud del firmware sin desconectar nada. Además alimenta la ESP con **5V (pin 1, OTG)
+ masa (pin 8)** del propio Flipper.

Sirve para reproducir en el banco el comportamiento en tiempo real (fixes a ~1 Hz) sin tener
que conducir — por ejemplo para diagnosticar el lag de la pantalla.

## Cableado

| Flipper | ESP32-S3 | Rol |
|---------|----------|-----|
| Pin 13 (TX / USART1) | GPIO18 | Datos GPS (TTL 3.3V) |
| Pin 1 (5V, OTG) | 5V in | Alimentación |
| Pin 8 (GND) | GND | Masa común |

> ⚠️ **No** alimentar la ESP por USB **y** por el Flipper a la vez (dos fuentes en
> conflicto). Usá solo el 5V del Flipper. La app habilita el OTG 5V automáticamente al
> arrancar y lo apaga al salir.

Pinout Flipper (GPIO superior): pin 1 = 5V, pin 8 = GND, pin 13 = TX (PB6).

## Formato de log esperado

Captura de terminal con timestamp por línea:

```
12:37:09.002, $GNRMC,153708.90,A,3429.49284,S,05832.90956,W,0.000,...*1B
12:37:09.002, ###IMEI869530045257306
```

La app quita el prefijo `HH:MM:SS.mmm, ` (14 bytes) y transmite el resto **tal cual**
(NMEA + IMEI + binario), con un `\n` al final. El timestamp se usa solo para **espaciar**
el envío y reproducir la cadencia real (~1 Hz). Las líneas de header sin timestamp se saltan.

## Build e instalación

Requiere [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) (micro Flipper Build Tool):

```sh
python3 -m pip install --upgrade ufbt

cd flipper/nmea_replay
ufbt                 # compila el .fap (dist/nmea_replay.fap)
ufbt launch          # con el Flipper por USB: compila + sube + abre la app
```

## Cargar los logs en el Flipper

Copiá los `.log` a la SD del Flipper en `/ext/` (raíz de la SD), por ejemplo con
[qFlipper](https://flipperzero.one/update) o:

```sh
ufbt cli   # luego:  storage write /ext/nmea3.log   (o usar qFlipper, más simple)
```

## Uso

1. Cablear Flipper ↔ ESP (tabla de arriba).
2. Abrir **NMEA GPS Replay** en el Flipper (categoría GPIO).
3. Elegir el `.log` en el explorador.
4. Reproduce en **loop continuo**. Pantalla: líneas/bytes enviados, tiempo, nº de loop, baud actual.
5. **◄/►** cambia el baud (9600 ⇄ 115200) sin interrumpir la reproducción.
6. **Back** detiene y libera todo (serial + OTG 5V).
