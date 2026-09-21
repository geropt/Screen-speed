# P05 — Conectividad y respaldo con ACK

Revisión: 2026-09-05. **No cierra P05**: la puerta del plan exige servidor de prueba,
ACK/NACK/timeouts probados en el enlace real y medir bytes ya en vuelo con tráfico
verdadero. Lo que hay acá es la lógica que decide y contabiliza, con pruebas, más el
dueño de la radio, sin verificar.

## 1. Lo que se hizo y lo que no se puede hacer todavía

| Pieza | Estado | Verificable en host |
| --- | --- | --- |
| `outbox` — pendiente / en vuelo / confirmado | Implementado y probado | Sí, con servidor simulado |
| `backup_policy` — permiso y revocación | Implementado y probado | Sí |
| Vectores independientes del protocolo | Implementados | Sí |
| `connectivity_manager` — dueño de la radio | Implementado | **No**: envuelve esp_wifi |
| `backup_cfg` — texto host/puerto/redes | Implementado y probado | Sí |
| Integración en el firmware | Tarea backup + `/sdcard/backup.cfg` | Parser sí; radio/TCP no |

La integración quedó afuera a propósito: conectar el respaldo al enlace real sin banco
de pruebas con IMEI propio significaría competir con el tracker operativo. El plan lo
prohíbe explícitamente.

## 2. La regla del outbox: no retirar por éxito de `send()`

Es el requisito textual del plan y la razón de que el componente exista. Que el socket
haya aceptado los bytes no significa que el servidor los tenga: puede caerse el enlace,
puede perderse el ACK, puede llegar un NACK.

```
pendiente --begin_batch--> en vuelo --confirm--> eliminado
                               |
                               +--release--> pendiente otra vez
```

`confirm` es lo único que borra, y sólo se llama con un ACK en la mano. El servidor
simulado de las pruebas distingue «se enviaron los bytes» de «el servidor confirmó»,
que es exactamente donde un outbox de dos estados pierde datos en silencio.

Decisiones y su motivo:

- **Un solo lote en vuelo.** El ACK del Ruptela no identifica qué lote confirma, así
  que con dos lotes simultáneos no habría forma de saber cuál se confirmó. El segundo
  `begin_batch` se rechaza en lugar de inventar una correlación.
- **`release` conserva la secuencia original**, así que los liberados se reenvían antes
  que lo encolado mientras estaban en vuelo.
- **Cola llena descarta el pendiente más viejo, contado.** Si la cola está llena es
  porque el enlace no funciona, y cuando vuelva importa dónde está el vehículo ahora.
  Configurable a rechazar el nuevo; nunca silencioso.
- **Lo que está en vuelo no se descarta jamás**, porque su ACK todavía puede llegar.
- **Un record no se trunca**: medio record es algo que el servidor rechazaría.
- **Bytes en vuelo medidos aparte** de los pendientes, como pide el plan.

## 3. La política de permiso

| Regla | Por qué |
| --- | --- |
| Sin IMEI conocido, no se envía | Sin identidad no se puede armar un paquete válido |
| GPRS desconocido no habilita | Si el tracker envía y el HUD también, se duplican los datos |
| GPRS vencido no habilita | Un IO 418 viejo no dice nada del presente |
| Dwell antes de tomar el relevo | Sin esto el respaldo entraría y saldría en cada túnel |
| Revocación inmediata al volver el GPRS | Competir por el IMEI es peor que perder unos records |
| Otro IMEI revoca y exige dwell desde cero | El permiso era para ese tracker |
| Ignición apagada revoca; desconocida deniega | No se asume que el vehículo está en viaje |

El dwell se reinicia si el tracker vuelve: hay una prueba que verifica que 11 s
acumulados **no** se suman al siguiente intento. La revocación queda pendiente hasta
que el transporte declara que cesó, y el tiempo que tardó es observable, porque la
meta del plan es ≤250 ms para cerrar el socket local.

### La limitación que no se puede tapar

Esta política **no** es una reserva exclusiva del IMEI ni detecta toda falla celular.
IO 418 es lo que el tracker *reporta* por RS232; entre que el tracker decide enviar por
GPRS y que el HUD se entera hay una carrera que ninguna política local puede cerrar. El
plan lo marca y esta implementación no lo resuelve: lo acota con dwell y revocación
rápida. Queda escrito en el encabezado del componente para que nadie lo lea como una
garantía.

## 4. El punto ciego de cmd68, cerrado

En P02a quedó registrado que el test de `cmd68` construía el paquete con
`ruptela_build_cmd68()` y lo verificaba con `ruptela_crc16()`: validaba la función
contra sí misma. Un roundtrip así pasa igual si el layout está equivocado de punta a
punta.

Ahora los bytes esperados se escriben a mano desde la especificación, y el CRC-16 se
compara contra una reimplementación independiente. Que dos implementaciones escritas
por separado coincidan es evidencia; que una se verifique consigo misma no lo es.

Escribir el vector obligó a leer el layout real del payload, que es
`[records_left][cantidad][records...]` — no lo que yo había supuesto. **El encoder
estaba bien; la suposición era mía.** Ese es justamente el tipo de error que un vector
independiente encuentra y un roundtrip no.

También quedó aseverado en código que el comando es `0x44` y que `0x68` está en la
lista de descarte, para que nadie «corrija» el valor creyendo que el plan pide
hexadecimal: el «68» del plan es decimal.

## 5. El dueño de la radio

`connectivity_manager` existe porque en el piloto el Wi-Fi se prende y se apaga desde
la misma lógica que arma paquetes y habla con el servidor. Ese diseño no soporta un
segundo consumidor, y en P08 y P09 aparecen dos.

- **Solicitudes por consumidor** (respaldo, mapas, OTA, diagnóstico). La radio se
  levanta con el primer pedido y se apaga sólo cuando no queda ninguno: eso evita que
  el respaldo corte una transferencia de mapas al terminar lo suyo.
- **Arranca apagada.** Un HUD que nunca necesita red no debería gastar energía en Wi-Fi.
- **Esperas cancelables**: `conn_cancel_waits()` hace que quien esperaba vuelva de
  inmediato, en lugar de aguantar el timeout del stack. Es el mecanismo que permite que
  una revocación interrumpa una espera en curso.
- **Sin borrado global de NVS ni credenciales de build**, las dos cosas que hace el
  piloto y que el plan prohíbe para el producto. Cada red es una clave propia.

## 6. Verificación ejecutada

| Qué | Resultado |
| --- | --- |
| `test_outbox` | 15 casos con ASan/UBSan, en verde |
| `test_backup_policy` | 14 casos, en verde |
| `test_ruptela_vectors` | 8 casos con bytes escritos a mano, en verde |
| Build del HUD | Compila; `libconnectivity.a` se construye |
| Las 8 suites de host | En verde |

## 7. Lo que falta para cerrar P05

1. **Servidor de prueba y banco con IMEI propio.** Sin eso no hay ACK/NACK real, ni
   medición de bytes en vuelo con tráfico verdadero, ni prueba de que el HUD siga usable
   con un servidor lento.
2. **Integrar el respaldo en el firmware**: hoy los tres componentes existen y nadie los
   llama. Falta la tarea que evalúe la política, arme lotes y hable con el servidor.
3. **Extraer el protocolo del piloto de verdad.** `ruptela_proto` ya está compartido,
   pero `telematics.c` sigue con su máquina de estados de agrupación por ciclo, mucho
   estado de archivo y cero pruebas. Esa lógica —índices, máscara de completitud,
   timeout de grupo— es no trivial y merece salir a un componente probado.
4. **Retorno 418=1 durante cada etapa**: el plan pide probar que la revocación cancela
   nuevos envíos en DNS, connect, send y ACK. La política ya lo decide y las esperas ya
   son cancelables, pero verificarlo exige el enlace.
5. **Endpoint, TLS, duplicados y antigüedad**: TLS sigue sin definir. El endpoint
   ya no va sólo en el binario: Kconfig es fábrica, NVS persiste, y
   `/sdcard/backup.cfg` lo pisa al montar la tarjeta (`host=`, `port=`,
   `ssid=`/`password=`). El cliente sigue siendo TCP plano.
6. **Journal durable**: el outbox vive en RAM, así que un reinicio pierde lo pendiente.
   El plan lo permite —«journal durable sólo si requerido»— pero es una decisión que
   hay que tomar explícitamente, con su tarea y presupuesto.
