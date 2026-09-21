# Integración Wi-Fi y backup de telemetría por Starlink

Estado: existe un piloto separado; integración con la pantalla pendiente.
Esta propuesta se integra en el [blueprint definitivo](blueprint-firmware.md)
y en P05 del [plan de implementación](plan-evolucion.md).
Fuentes principales: [piloto](../../starlink_pilot/README.md),
[telematics.c](../../starlink_pilot/main/telematics.c),
[protocolo C](../../starlink_pilot/main/ruptela_proto.c) y
[notas de captura/replay](../../../starlink-backup.md).

## Objetivo

El HUD recibe los records del Ruptela por UART y, cuando la política permite el
backup, los envía al servidor del vehículo mediante Wi-Fi con salida por Starlink.
Usa el IMEI real del tracker y conserva el record binario y su timestamp. NMEA
alimenta el display; no se reconstruye desde NMEA un record para el servidor.

Starlink es una conexión IP de salida: el firmware cliente no necesita una API de
la antena. El servidor y las credenciales deben ser configuración de la unidad.
Los valores de laboratorio en el piloto no son defaults de una flota.

## Qué reutilizar del piloto

| Pieza | Estado observado | Acción de integración |
| --- | --- | --- |
| `ruptela_proto.c/.h` | CRC16, cmd 68, heartbeat 16, parsing de respuestas y lista de comandos no soportados | Extraer como componente puro C con sus tests |
| Parser IO | Callbacks para 409, 418 y record completo; validación de estructura/cabecera | Unificar con HUD y documentar límite de frame recibido frente a record reenviable |
| Recepción UART | Drena `UART_DATA`; detecta `###IMEI` entre chunks | Reutilizar la mejora; corregir decoder NMEA que todavía corta en NUL |
| Agrupación | Agrupa records por timestamp e índices de extensión; timeout 500 ms | Probar ciclos completos, parciales, repetidos y fuera de orden |
| Wi-Fi | STA con hasta tres redes configuradas en build | Mover a `connectivity` y configuración persistente |
| TCP | DNS, conexión, envío, recepción y heartbeat | Hacer cancelable; agregar retención hasta ACK y timeouts de protocolo |

El scanner del piloto acepta longitud de entrada de hasta 255 B, mientras su
validación de record extended reenviable limita a 126 B. No unificar constantes
sin conservar esta distinción y los tests de colisiones CRC.

Los tests host ejecutados durante esta documentación pasan: ocho de IO y cuatro
de protocolo, con ASan/UBSan. Esto no valida red, handover ni convivencia con LVGL.

## Contrato de protocolo a preservar

Según el código y los vectores del repositorio, el camino de salida es:

```text
UART: longitud + record binario + CRC8
                  ↓ validación
TCP: longitud + IMEI uint64 + cmd 68 + contador(es) + records + CRC16
```

El piloto define máximo de paquete 1024 B y hasta ocho records por paquete;
también debe respetarse el máximo real de bytes al agrupar. Construye heartbeat
16 y parsea respuestas 100/116. El CRC y su orden de bytes deben conservar los
vectores de `host_tests/test_ruptela_proto.c`, no reinterpretarse a partir de una
descripción textual.

La documentación histórica remite al protocolo Ruptela 1.133. Antes de declarar
compatibilidad de producción, fijar su revisión y archivar vectores/capturas
autorizadas. Las notas reportan ACKs reales, pero no prueban todos los comandos ni
garantizan que un comando rechazado quede pendiente para el tracker celular.

No ejecutar desde el HUD comandos de configuración, FOTA, SMS o Set IO destinados
al Ruptela. El piloto cierra ante 102/104/108/117. Verificar el comportamiento de
reintento del servidor sin asumirlo. La futura actualización del firmware HUD
será un servicio separado.

## Permiso de envío y frescura de IO 418

IO418 significa estado GPRS (0 desconectado, 1 conectado), según
[Ruptela](https://my.ruptela.com/en/articles/14178996-how-to-troubleshoot-internet-connection-on-devices).
No confirma una sesión TCP exclusiva ni que el servidor haya aceptado los records.
Por lo tanto, esta política no detecta todos los problemas de entrega celular:
por ejemplo, con 418=1 y servidor inaccesible no habilitaría el backup. Cubrir ese
caso requiere otra señal documentada o coordinación del backend.

El permiso es una política local, no una reserva de conexión reconocida por el
tracker o servidor. La nota de laboratorio reporta conflicto de conexiones con el
mismo IMEI. Sigue existiendo una carrera durante el intervalo entre reconexión
celular del Ruptela y recepción de su siguiente IO 418.

| Estado propuesto | Condición | Acción |
| --- | --- | --- |
| DISABLED | Configuración deshabilitada | No enviar; liberar solicitud de red propia |
| UNKNOWN | Sin IMEI válido o IO 418 ausente/inválido/vencido | Cancelar envíos y cerrar socket |
| CELLULAR_UP | IO 418 fresco igual a 1 | Revocar inmediatamente el permiso, sin debounce |
| DOWN_DWELL | IO 418 fresco igual a 0 por menos de `T_down` | Esperar confirmaciones frescas |
| BACKUP_ALLOWED | Down sostenido y condiciones de producto cumplidas | Solicitar red y conectar |
| BACKOFF | Error de red, ACK o servidor, conservando permiso | Reintentar con espera acotada y cancelable |

El piloto usa `T_down=15 s`; es un punto de partida de laboratorio. Falta definir
`T_io_stale` según frecuencia real de publicación de 418. Un solo 418=0 seguido
de silencio no habilita backup al transcurrir 15 s. El vencimiento debe ejecutarse
por timer aunque no llegue ningún evento nuevo.

La nota original requiere ignición ON; el piloto la hace opcional y la desactiva
por defecto. La propuesta para HUD conserva el requisito de ignición fresca ON
para el primer corte. Confirmar esta decisión antes de liberar el producto.

Cerrar el socket desde su tarea propietaria mediante señal prioritaria. Todas las
esperas de DNS/conexión/ACK deben tener límites; verificar nuevamente permiso,
identidad y frescura después de conectar y antes de cada envío. No basta la
comprobación al comienzo del loop actual.

## Entrega de paquetes

`send()` exitoso no equivale a ACK del servidor. En el piloto los ACK se loguean,
pero no gobiernan el retiro de registros. Para la integración:

1. Mantener una cola acotada por registros, bytes y antigüedad; separar pendientes
   del lote en vuelo. Un solo lote en vuelo simplifica correlación con ACK 100.
2. Conservar el lote hasta recibir un ACK válido con resultado aceptado. Tratar
   NACK, CRC inválido, desconexión y timeout como estados distintos.
3. Al perder ACK después de un envío puede haber duplicados al reintentar.
   Documentar semántica de entrega y probar deduplicación/orden en el backend;
   no prometer exactamente una entrega.
4. Definir qué se conserva durante servicio celular normal y qué se descarta por
   overflow, vencimiento o cambio de IMEI. No reenviar automáticamente un backlog
   que ya pudo viajar por celular. Agrupar usando identidad y sesión además de UTC.
5. Si se requiere sobrevivir a reinicios, agregar journal en SD con presupuesto y
   recuperación. Sin journal, declarar explícitamente pérdida de pendientes al
   apagar. La recepción UART jamás espera por el journal.

El backoff de la nota original todavía no está implementado como política de ACK
en el piloto. Ajustar sus intervalos contra protocolo/backend y medir latencia de
cancelación al volver 418=1. El cliente actual usa TCP; TLS con validación de
certificado, hora y configuración del endpoint es trabajo pendiente.

## Un solo responsable de Wi-Fi

`connectivity` administra inicialización, STA/SoftAP, credenciales y reconexiones.
Los clientes solicitan conectividad; no llaman independientemente a
`esp_wifi_stop`, `esp_wifi_disconnect` o a la inicialización global.

Cerrar telemetría al volver el celular no debe desconectar a una app que está
instalando mapas. Que exista conexión Wi-Fi tampoco habilita por sí solo enviar
records del vehículo. La habilitación del socket y el uso de la radio son estados
separados.

Para el primer corte, usar modos de operación explícitos: normal/backup y
mantenimiento con teléfono. Concurrencia AP+STA queda detrás de pruebas en placa:
ambas interfaces comparten canal y el canal de STA tiene prioridad, según
[ESP-IDF Wi-Fi](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/api-guides/wifi.html).
No asumir dos enlaces de radio independientes.

El mantenimiento no debe suspender silenciosamente un backup requerido. Si no se
puede sostener la transferencia junto a telemetría, pausar la instalación y
comunicarlo a la app; los bytes confirmados permanecen recuperables.
