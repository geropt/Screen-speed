# P02a — Framing compartido del Ruptela con entrada por longitud

Revisión: 2026-09-05. Primera mitad de P02 del [plan](plan-evolucion.md): el
framing pasa a ser compartido, seguro frente a datos binarios, y nada se publica
antes de validar. La segunda mitad —`tracker_epoch`, edades por señal, snapshot
coherente y arranque sin SD— es P02b.

## 1. El problema concreto que había

El HUD trataba el enlace del Ruptela como texto ASCII. Medido sobre
`main/nmea_parser.c` de la línea base:

- La única rama que leía datos era `UART_PATTERN_DET`, con el patrón `'\n'`
  configurado en `uart_enable_pattern_det_baud_intr(port, '\n', 1, 9, 0, 0)`.
  **`UART_DATA` estaba vacía**: un `break` sin hacer nada.
- El recorrido del bloque leído era `while (*d)`, con un `'\0'` forzado al final,
  así que **un 0x00 en los datos cortaba el resto del bloque**.
- El fin de sentencia se decidía en `'\r'` y el inicio en `'$'`, que además
  **reiniciaba todo el estado de parseo**.

El enlace es un canal transparente que transporta, mezclados: sentencias NMEA,
el marcador `###IMEI<dígitos>` y records binarios de IO. Con ese diseño, tres
bytes perfectamente normales dentro de un payload binario rompían el transporte:

| Byte | Qué provocaba |
| --- | --- |
| `0x24` (`$`) | Reiniciaba el parseo a mitad de una sentencia en curso |
| `0x0A` (`\n`) | Disparaba una detección de patrón espuria que partía el frame |
| `0x00` | Cortaba el recorrido y descartaba el resto del bloque leído |

El piloto ya había resuelto esto por su lado: drena todo sin esperar
delimitadores y reconoce los records **por longitud declarada**. P02a unifica
ambos caminos en un componente y le da al HUD el mismo transporte.

## 2. Componente compartido

`esp/components/ruptela_framing`, C puro, sin ESP-IDF, compilado por los dos
proyectos vía `EXTRA_COMPONENT_DIRS`:

| Archivo | Origen | Qué hace |
| --- | --- | --- |
| `ruptela_io_parser.c/h` | movido del piloto, sin cambios | Records de IO por longitud declarada + CRC8 |
| `ruptela_proto.c/h` | movido del piloto, sin cambios | Paquetes de dispositivo, CRC16, ACK |
| `nmea_framer.c/h` | nuevo | Framing de sentencias NMEA seguro frente a binario |
| `imei_scanner.c/h` | nuevo | Marcador `###IMEI` con arrastre entre lecturas |

Los dos primeros se movieron con `git mv`, sin editar una línea: ya eran puros y
ya tenían pruebas en host, que siguen pasando desde su nueva ubicación.

### Entrada por longitud, que ya existía

El formato en el cable es `[longitud 1 B][record][CRC8 1 B]`. El parser no cierra
un frame hasta tener `1 + longitud + 1` bytes, y además exige que los grupos de IO
consuman exactamente `record_len` y que el encabezado GNSS sea plausible antes de
aceptarlo. El CRC8 solo no alcanza: son 8 bits y el texto NMEA colisiona seguido,
por eso hay validación estructural encima.

### El framer NMEA: la sentencia termina por estructura

La decisión de diseño central: **una sentencia se cierra en `'*'` seguido de
exactamente dos dígitos hexadecimales**, no en un newline. Ahí se compara el
checksum XOR y sólo entonces se entrega. El CR/LF posterior es ruido irrelevante,
y un flujo que nunca traiga `'\n'` funciona igual.

El resto de las reglas:

- `'$'` abre sentencia siempre; si había una a medio armar, la descarta y lo
  cuenta (`aborted_restart`).
- Un byte de control o `0x00` dentro de una sentencia sin cerrar la aborta
  (`aborted_control`).
- Pasarse del máximo la descarta (`overflow`) y el framer vuelve a esperar `'$'`.
- Un carácter no hexadecimal donde va el checksum la descarta
  (`bad_checksum_char`).
- Los bytes fuera de toda sentencia se cuentan (`noise_bytes`) y no acumulan nada.

Cada categoría tiene su contador porque «no llega nada» y «llega roto de esta
forma» son fallas distintas y en campo hay que poder distinguirlas.

### Corrección de nomenclatura: «cmd68» es 0x44

El plan menciona «cmd68» junto a CRC y heartbeat. En el código el comando de
records extendidos es `RUPTELA_CMD_RECORDS_EXT = 0x44`, y **68 es decimal**:
0x44 = 68. En hexadecimal `0x68` = 104 = `RUPTELA_CMD_FOTA`, que está en la lista
de comandos a descartar. Queda registrado para que nadie implemente 0x68 creyendo
que sigue el plan.

## 3. Qué cambió en el HUD

| Antes | Ahora |
| --- | --- |
| `UART_DATA` vacía; sólo leía en `UART_PATTERN_DET` con `'\n'` | `UART_DATA` drena todo; también se drena al vencer el timeout de cola |
| Una posición de patrón por evento | Bucle hasta vaciar el ring, con `uart_read_bytes` sin bloquear (timeout 0) |
| `while (*d)` sobre buffer terminado en NUL | Bytes crudos al framer; ningún terminador |
| Fronteras `'\n'` / `'\r'` / NUL | Frontera por estructura de la sentencia |
| Overflow del ring: `flush` y seguir | `flush` + descartar la sentencia parcial: los bytes perdidos no se pegan con los siguientes |
| Publicaba cualquier RMC con checksum válido | Publica sólo con status `'A'` y coordenadas finitas en rango |
| Retorno de `esp_event_post_to` ignorado | Cola llena se cuenta como `RES_ERR_QUEUE_FULL` y se loguea |
| Sin instrumentación | Intervalo entre drenajes en `RES_CH_UART_DRAIN`, errores por categoría, stack de la tarea vigilado |

El parseo de campos **no se tocó**. El framer entrega la sentencia y se le agrega
un `'\r'` para que la máquina de estados existente la cierre igual que antes; el
cambio se limita al transporte. Eso implica que el checksum se verifica dos veces,
a propósito, y esa redundancia se saca cuando el parseo de campos se extraiga en
P02b.

Se detectó y corrigió un problema de re-entrada durante la implementación: el
callback del framer no puede escribir en el mismo buffer que el framer está
recorriendo, porque corrompería el resto del bloque leído. La sentencia se copia a
un buffer propio (`sentence_buf`, dentro de la struct ya asignada en heap).

### Cambio de comportamiento visible

Antes, un RMC con status `'V'` —receptor sin fix— publicaba igual, con posición 0
o con la anterior, y disparaba un barrido completo de 9 tiles más las lecturas de
SD correspondientes. Ahora no se publica y se cuenta en `rejected_invalid_fix`.

Consecuencia a tener presente hasta P03: mientras no haya fix válido, la pantalla
**conserva el último valor** en lugar de recibir uno inventado. Mostrar
explícitamente «desconocido / vencido / último conocido» es tarea de P03, y es la
razón por la que esa etapa existe.

## 4. Pruebas

`host_tests/test_ruptela_framing.c`, 24 casos con ASan/UBSan, cubriendo la fila
«Framing» de la matriz mínima de fallas del plan:

| Caso | Qué prueba |
| --- | --- |
| Sentencia válida sin newline | El flujo no contiene `'\n'`, `'\r'` ni NUL y se reconoce igual |
| CR/LF posterior | No aborta nada; cuenta como ruido fuera de sentencia |
| Mensajes concatenados | Dos sentencias pegadas sin separador alguno |
| Corte en cada offset | La misma sentencia partida en dos entregas, en las `n+1` posiciones |
| Corte byte a byte | Entregas de un byte |
| Binario con NUL/newline/`$` | Bloque con 0x00, 0x0A, 0x0D y varios 0x24, seguido de sentencia buena: llega intacta |
| `$` a mitad | Reinicia y cuenta; la sentencia siguiente se entrega |
| Ruido largo | 4 KiB de ASCII sin `$`: no acumula ni pierde la sentencia posterior |
| Overflow | Cuerpo de 300 B sin `*`: se descarta contado y no impide la siguiente |
| CRC malo, cuerpo corrompido, checksum no hex, minúsculas | Publicación sólo con checksum coincidente |
| `discard_partial`, `reset`, punteros nulos | Ciclo de vida |
| IMEI: corte en cada offset, 16 dígitos, demasiado corto, prefijo solapado, dos marcadores, entre binario | Reconocimiento con arrastre |

El caso del prefijo solapado (`#####IMEI…`) encontró un error real en la primera
implementación del scanner: el reintento de un solo carácter no cubre solapamientos.
Se reemplazó por una ventana deslizante, que es correcta por construcción.

### Verificación completa ejecutada

| Qué | Resultado |
| --- | --- |
| Build del HUD | Compila. Binario 1 671 280 → 1 671 440 B (**+160 B**), DIRAM sin cambio, IRAM sin cambio (16 383 de 16 384) |
| Build del piloto | Compila con el componente compartido |
| `make -C esp/Firmware/host_tests test` | 3 suites en verde, incluida la caracterización del matcher, que no se movió |
| `make -C esp/starlink_pilot/host_tests test` | Las 12 pruebas movidas siguen pasando |

## 5. Lo que sigue abierto

- **Nada de esto se probó contra un Ruptela real.** Hace falta banco con IMEI
  propio; el tracker operativo no se toca. La captura de un stream mixto real
  serviría para agregar un caso de regresión con datos de campo.
- El HUD todavía **no consume** IO 409/418 ni el IMEI: tiene el framing
  disponible, pero conectar señales con edad propia es P02b.
- El parseo de campos NMEA sigue acoplado a `esp_gps_t` y sin pruebas propias;
  extraerlo es parte de P02b, y ahí desaparece la doble verificación de checksum.
- Sigue faltando un vector independiente de `cmd68` (0x44): el test actual sólo
  hace ida y vuelta con la misma función de producción, así que no ata el formato
  a lo que espera el servidor real. El heartbeat sí tiene vector dorado.
- `telematics.c` del piloto sigue sin pruebas y con mucho estado de archivo; su
  lógica de agrupación por ciclo es no trivial. Cae en P05.
