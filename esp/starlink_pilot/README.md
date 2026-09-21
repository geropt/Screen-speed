# Starlink pilot (ESP32-WROOM)

Firmware headless, **aparte** de la HUD (`esp/Firmware`). Recibe el RS232→TTL del Pro5/HCV5 como hoy (NMEA + `###IMEI` + records IO) y, cuando IO 418 lleva un rato en 0, abre TCP command 68 al mismo host:puerto de flespi.

No toca mapas, LVGL ni la pantalla.

## Cableado (WROOM)

El conversor RS232→TTL es el de siempre, lógica 3V3.

| Ruptela (TTL) | ESP32-WROOM |
|---------------|-------------|
| TX            | GPIO16 (UART2 RX) |
| GND           | GND |

No uses UART0 (USB) ni UART1 (GPIO9/10, flash). Alimentá el DevKit por USB.

En un S3 DevKit sin display: `idf.py set-target esp32s3` y RX en GPIO18 (`idf.py menuconfig` → NMEA Configuration).

## Config

```sh
cd esp/starlink_pilot
idf.py set-target esp32
idf.py menuconfig   # Starlink telematics: SSID, PSK, host, port
idf.py build flash monitor
```

Defaults en `sdkconfig.defaults`:

- WiFi: `Personal-916-2.4GHz` y `ClaroRS` (2.4 GHz). Password vacío hasta menuconfig.
- `46973.flespi.gw:29043`
- Dwell GPRS-down 15 s
- Ignición (409) no bloquea el TCP

El IMEI **no** se configura: sale del `###IMEI` del UART.

## Cuándo conecta

1. Hay IMEI.
2. Llegó IO 418 (si no aparece, no adivina: no hay TCP).
3. 418 = 0 durante `T_down`.
4. STA al Mini / AP de lab, TCP cmd 68 con el record crudo, ACK 100.

Si 418 vuelve a 1, suelta el socket. Si flespi manda 102/104/108/117, cierra sin ACK.

Para ver el device en el mapa por este camino, el celular del Pro5 tiene que estar abajo de verdad (SIM afuera, GPRS off, o sin cobertura). Mismo IMEI + mismo canal = error 12 si los dos hablan.

## Tests de protocolo (sin IDF)

```sh
make -C esp/starlink_pilot/host_tests test
```
