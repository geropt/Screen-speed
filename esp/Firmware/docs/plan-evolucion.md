# Plan de implementación, pruebas y aceptación

Revisión: 2026-09-05. Plan propuesto para ejecutar el
[blueprint del firmware](blueprint-firmware.md), no registro de tareas terminadas.
No se asignan fechas sin la línea base en Waveshare. Cada etapa debe producir un
firmware comprobable o un componente con pruebas, no solo carpetas nuevas.

Decisión de alcance del usuario: evaluar v2/zonas después, sin bloquear ninguna
integración. Conservar geometría v1 y proponer una [cápsula](capsula-mapas.md)
para actualizar sin depender de un filesystem de tiles. El checklist operativo
está en [backlog de tareas](tareas-firmware.md); este documento conserva el detalle
de pruebas y aceptación de cada etapa.

## 1. Estrategia y orden

Primero proteger la función existente y liberar margen de recursos. Después
integrar conectividad, instalación y app sobre contratos estables. Separar siempre:
extracción de código, corrección de comportamiento, optimización y upgrade de SDK.

| Paquete | Entregable | Depende de | Puerta de aceptación |
| --- | --- | --- | --- |
| P00 | Baseline reproducible y dossier de unidad | — | Código/build/dataset/placa identificados |
| P01 | Servicios de placa y presupuesto medido | P00 | Panel validado; memoria/latencia comparadas |
| P02 | Recepción y estado de vehículo compartidos | P00 | Flujo mixto robusto; frescura y arranque sin SD |
| P03 | UI encapsulada y modelo de presentación | P01, P02 | Único escritor; velocímetro independiente de SD |
| P04 | Matcher puro y map_store con generación | P00, P02 | Replay v1 equivalente en extracción |
| P05 | Conectividad y respaldo con ACK | P01–P03 | UART/UI sobreviven a fallas de red; permiso cancelable |
| P06 | Investigación opcional de mapas/zonas | P04; aprobación para iniciar | Comparación medida y decisión, no obligación de migrar |
| P07 | Instalador de cápsulas con payloads actuales | P04 | Cliente de banco; cortes no activan candidata inválida |
| P08 | Pairing y app de teléfono | P05, P07 | Transferencia autenticada, reanudación y revocación |
| P09 | OTA A/B con autoprueba | P01–P03, transporte de P05/P08 | Imagen fallida revierte; activa sobrevive a corte |
| P10 | Provisión y seguridad de producción | P08, P09 | Firma, claves, recuperación y compatibilidad aprobadas |
| P11 | Calificación integrada y liberación | Etapas incluidas en release | Evidencia sostenida de recursos, campo y recuperación |

P04 extrae el lector/matcher actual sin adoptar otro formato. Las ideas de
portabilidad del [borrador v2](../../../.kiro/specs/tile-format-v2/tasks.md) pueden
servir como referencia, no como requisitos aceptados. P06 queda diferido y no
bloquea ninguna integración. La cápsula se desarrolla dentro de P07; su backend
no es requisito para extraer P04 ni integrar UART, UI, Wi-Fi, Starlink u OTA.
Después de los contratos base, almacenamiento y
conectividad pueden avanzar como líneas separadas con integración frecuente.
El instalador puede probarse antes de la app. OTA puede probarse con cliente de
banco; no depende de tener una app comercial terminada.

## 2. Paquetes de trabajo detallados

### P00 — Congelar y poder reproducir lo que hoy funciona

Trabajo:

- Registrar cambios locales existentes sin mezclarlos con el refactor. Identificar
  commit, dirty state, build ID, SDK, toolchain y dependencias de la referencia.
- Registrar SKU/revisión de Waveshare, flash/PSRAM reales, panel, tarjeta y
  alimentación. Resolver variante -G antes de reutilizar GPIO18.
- Registrar modelo/FW/CFG del Ruptela, conversor, baud, frecuencia de NMEA/IO y
  capturas autorizadas. Conservar secretos fuera de fixtures/versionado público.
- Guardar dataset exacto, su generador/configuración y replays representativos.
- Unificar defaults 16 MB y layout después de verificar hardware; ensayar build
  desde configuración limpia en directorio nuevo, sin borrar la del usuario.
- Identificar fuente EEZ, versión de editor/exportación y baseline visual.

Cierra con: build reproducible, inventario, hashes y logs de referencia; tests
actuales ejecutados y limitaciones anotadas. No aprobar solo porque compila.

### P01 — Placa, memoria y display bajo control

Trabajo:

- Extraer pines/capacidades y panel a `board_waveshare_175`, conservando secuencia
  funcional. Comparar controlador físico/CO5300 con driver SH8601 existente.
- Añadir medición de heap interno/DMA/PSRAM, mayor bloque, stacks, colas, tiempo
  de render/flush y frecuencia de errores. Logs acotados, sin tráfico por píxel.
- Parametrizar buffers de 116/64/48/32 filas. Comparar con misma UI y replay,
  sin cambiar a la vez LVGL, algoritmo ni reloj del panel.
- Elegir estrategia A del [análisis de memoria](hardware-y-memoria.md); staging
  PSRAM es experimento posterior si hace falta, no requisito para cerrar P01.
- Preparar un dueño del bus I2C para touch/PMIC/RTC. Incorporar periféricos uno
  por uno, incluyendo errores y ausencia; no habilitar audio/IMU por defecto.

Cierra con: tabla de mediciones antes/después, colores/áreas correctos, sin
reutilización prematura de buffers, ausencia de fugas al apagar/encender y margen
interno demostrado. El ahorro potencial con 32 filas es 152,91 KiB, no un ahorro
ya medido. Un cambio de controlador o BSP necesita su propia comparación visual.

### P02 — Ruptela y estado confiables

Trabajo:

- Extraer framing NMEA/IO/IMEI compartido por HUD y piloto, con entrada por longitud.
- Drenar UART_DATA y timeout; quitar dependencia de newline/NUL como frontera de
  transporte. Conservar la distinción scanner/record extended.
- Publicar solamente después de checksum, estructura y validación de campos.
- Sustituir callbacks que acceden a UI/SD por mensajes acotados; corregir API de
  cola usada desde tarea y comprobar todos los retornos de inicialización.
- Introducir `tracker_epoch`, reloj monotónico, señales IO con edad propia y
  snapshot coherente. No renovar 418 porque llegó otro record.
- Arrancar UART/estado sin SD y recuperar SD posteriormente sin reset global.
- Separar mailbox GPS de outbox; medir ring de 8 KiB y máximos de caudal.

Cierra con: mismo parser compilado en ambos targets, tests sanitizados y stream
mixto con cortes en todos los offsets, ruido/CRC/overflow/NUL/newline. GPS vencido
y cambio de IMEI invalidan lo necesario. Ningún callback UART espera display,
DNS, ACK ni escritura SD.

### P03 — UI mantenible y velocidad independiente

Trabajo:

- Crear `ui_model`, presenter, navegación y acciones; migrar `vars`, `dynamic`,
  splash y estados de panel sin cambiar primero el diseño visual.
- UI es único escritor de widgets; mantener excepción del puerto para flush-ready.
- Sacar tolerancia/histéresis/alerta del código visual hacia lógica pura testeable.
- Mostrar velocidad al recibir estado válido, antes de que termine matching.
- Implementar desconocido/vencido/inferido/último conocido; retirar límite inicial
  ficticio. Definir comportamiento ante ignición desconocida y modo servicio.
- Regenerar desde EEZ y comprobar que no borra lógica manual ni introduce drift.
- Añadir shell mínimo de pantallas de estado/mantenimiento, sin servicios falsos:
  una función no disponible debe aparecer deshabilitada o no aparecer.

Cierra con: UI anterior preservada en estado normal, screenshots/fotos de estados
de falla, navegación sin referencias colgantes, velocidad fluida con SD lenta y
sin llamadas de widgets desde workers. Trazar recepción→snapshot→flush completado.

### P04 — Extraer matching sin alterar resultados

Trabajo:

- Extraer el código actual: mismo C para host/ESP, fuente de candidatos desacoplada,
  estado explícito y reset. Conservar matemática/parámetros de referencia.
- Crear `map_result` con origen de fix/dataset y estados tipados.
- Extraer caché y ciclo de vida a `map_store`; especificar ownership de tiles,
  lectores activos, generación y errores de almacenamiento.
- Añadir replay determinista que compara antes/después y resetea entre logs.
- Después de demostrar equivalencia, corregir en cambios separados caché negativa,
  OOM/expulsión, archivos truncados y falta de reset, con tests que expliquen cambios.

Cierra con: equivalencia v1 de extracción en corpus acordado, resultado rechazado
si pertenece a generación vieja y pruebas de SD ausente/remount/OOM. No cerrar una
extracción aceptando diferencias bajo el argumento de que «ahora parece mejor».

### P05 — Integrar Wi-Fi/Starlink como servicio

Trabajo:

- Extraer protocolo puro del piloto y conservar vectores independientes de CRC,
  cmd68, heartbeat, ACK y límites de record/paquete.
- Crear `connectivity_manager`: único dueño de radio/netif/configuración, redes
  persistentes, solicitudes por consumidor y reconexión con backoff.
- Crear política de permiso con frescura, dwell, ignición e identidad; revocación
  prioritaria independiente de outbox. Documentar limitación de IO418/GPRS.
- Separar outbox, lote en vuelo y confirmación; no retirar por éxito de `send()`.
- Hacer DNS/connect/send/ACK cancelables o aislar sus esperas; revalidar permiso
  después de conectar y antes de enviar.
- Definir endpoint/TLS, política de duplicados, antigüedad y pérdida al apagar.
  Journal durable solo si requerido, con tarea/presupuesto y recuperación propios.
- No copiar al producto borrado global NVS ni credenciales de build del piloto.

Cierra con: servidor de prueba, ACK/NACK/timeouts probados, retorno 418=1 o stale
durante cada etapa cancela nuevos envíos; medir también bytes ya en vuelo. El
HUD sigue usable con Wi-Fi que no conecta, servidor lento y cola llena. La política
no se presenta como reserva exclusiva de IMEI ni detección de toda falla celular.

### P06 — Investigación opcional de mapas agrupados / zonas

Trabajo:

- Iniciar solamente cuando se priorice esta investigación, después de ordenar
  el firmware. Registrar el problema real: cantidad de archivos, latencia,
  actualización, tamaño o exactitud; no suponer que el formato explica todos.
- Comparar geometría actual en cápsula y propuesta v2/zonas
  sobre el mismo dataset y tarjeta. Revisar críticamente el borrador existente.
- Separar mejoras de geometría/precisión de cambios de almacenamiento.
- Preparar un experimento acotado únicamente si las mediciones lo justifican.
- Proponer conservar, ajustar o migrar, con costes y compatibilidad; obtener
  aprobación antes de implementar una migración.

Cierra con: informe comparativo y decisión explícita. «Mantener tiles» es un
resultado válido. No construir generador/lector v2 por obligación de este plan.

### P07 — Instalador de mapas antes de la app

Trabajo:

- Fijar manifiesto, firma/serialización, región, compatibilidad, tamaños y errores.
- Empaquetar los payloads actuales sin cambiar su binario: cápsula con índice
  coordenadas→offset/longitud, hashes y parámetros del lector existente. Añadir
  backend de lectura directa por rangos y demostrar equivalencia con directorio.
  No extraer un árbol de tiles ni exigir v2 para actualizar. Detalle en
  [cápsula de mapa](capsula-mapas.md).
- Implementar `begin/write/status/finish/activate/cancel` con cliente de banco.
- Usar paths internos, límites de offset/tamaño y chunks idempotentes; distinguir
  recepción de confirmación durable. Una actualización activa por vez.
- Recibir en candidata, reservar espacio, verificar completamente y reabrir con
  lector real antes de marcar READY.
- Implementar selección recuperable y barrera de lectores, cambio de generación,
  invalidación y reset. No asumir atomicidad NVS+SD.
- Recuperar después de corte, tarjeta cambiada, candidata dañada o fallback no
  compatible. Limpiar solo contenido inactivo y elegible.

Cierra con: fault injection en cada transición persistente, tests de corte físico
en la tarjeta elegida y región activa siempre verificable o estado explícito sin
mapas. SD corrupta no impide mostrar velocidad. Paquete inválido nunca queda activo.

### P08 — Teléfono y experiencia de mantenimiento

Trabajo:

- Definir Android/iOS y flujo de dueño/servicio/recuperación antes de cerrar UX.
- Pairing BLE con posesión única, ventana temporal, límites y revocación; luego
  solicitar mantenimiento desde la app vinculada sin configurar Wi-Fi en el HUD.
- Vincular la sesión BLE/provisioning con el canal bulk cifrado/autenticado;
  no exponer upload HTTP abierto porque BLE ya autenticó otro canal.
- Negociar `api_version` y capacidades; descubrir por BLE o mecanismo local
  aprobado sin asumir que todos los AP permiten tráfico entre clientes.
- App descarga paquete con Internet y permite transferir completo por BLE con
  fragmentación y créditos acotados. Ofrece Wi-Fi local guiado como modo rápido
  según duración estimada; UI separa recibir, verificar, listo y activar.
  Reanuda desde offset durable reportado por HUD, incluso al cambiar de enlace
  si la capacidad está disponible; un solo transporte escribe por sesión.
- Probar SoftAP sin Internet, app en background, bloqueo del teléfono, cambio
  de red, permiso denegado, teléfono perdido/revocado y cliente de versión anterior.

Cierra con: casos end-to-end en teléfonos reales, sesiones ajenas rechazadas,
sin secretos en logs, progreso recuperable y convivencia/política de pausa frente
al respaldo. Deshabilitar BLE temporalmente no impide volver a emparejar según
el ciclo de vida acordado.

### P09 — OTA con retorno seguro

Trabajo:

- Confirmar layout, tamaño final firmado y bootloader realmente instalado con
  rollback; separar provisión inicial de OTA de aplicación.
- Implementar streaming a slot inactivo desde HTTPS o cliente local autenticado.
- Verificar imagen/modelo/tamaño/versión/hash/firma y política de energía antes
  de seleccionar boot. Mantener A intacta ante cancelación o fallo de B.
- Implementar autoprueba acotada y confirmación; ausencia de Internet/GPS/SD no
  equivale a imagen defectuosa. Probar que fallos internos sí disparan rollback.
- Mantener NVS y mapas compatibles con aplicación anterior durante transición.
- Medir pausas de flash, UART, watchdog y PSRAM. No prometer OTA en conducción
  ni reanudación tras reboot sin implementarlas y ensayarlas explícitamente.

Cierra con: cortes durante descarga/erase/write/selección/primer boot, imagen
incorrecta/oversize/firma mala rechazadas y B defectuosa vuelve a A. Volver a A
también recupera configuración/mapa compatible. Un segundo OTA no pisa rollback
antes de que el anterior haya sido confirmado.

### P10 — Provisión y seguridad de producto

Trabajo:

- Threat model práctico: vecino de radio, teléfono revocado, SD manipulada,
  servidor/paquete falso, acceso físico y filtración de credenciales.
- Definir custodia/rotación de claves de firmware y mapas, identidad por unidad,
  credenciales de pairing, configuración cifrada y diagnóstico sanitizado.
- Decidir Secure Boot/flash encryption/NVS y recuperación en unidades de ensayo.
  No quemar eFuses ni desactivar USB/debug en la unidad de desarrollo sin aprobación.
- Definir anti-rollback y efecto sobre fallback, service tools y versiones mínimas.
- Ensayar provisión repetible, verificar resultado y documentar procedimiento
  para unidades existentes y nuevas; clave privada nunca va al dispositivo/repo.

Cierra con: paquete de producción verificable, recuperación ensayada dentro del
modelo de seguridad y revisión de operaciones irreversibles. Firmar un manifiesto
sin verificarlo en el dispositivo no satisface esta etapa.

### P11 — Calificación integrada y operación

Trabajo:

- Ejecutar matriz combinada sobre la Waveshare final con fuente, carcasa, tarjeta
  y Ruptela elegidos; repetir ciclos de encendido y cambios de servicios.
- Soak inicial de 24 h y luego 72 h con red intermitente, replay y aperturas/cierres
  de mantenimiento; estos plazos son objetivos de prueba, no certificación.
- Campo controlado con referencia de rutas/límites, cobertura y handover reales.
- Pruebas eléctricas/térmicas del producto; identificar límites de uso y soporte.
- Release checklist con dependencias, tamaño, schemas, evidencia y recuperación.

Cierra con: métricas dentro de umbrales acordados, sin fallas no explicadas, sin
fugas sostenidas ni pérdida propia de UART en perfil soportado. Guardar resultados
por hardware/build/dataset; no generalizar un ensayo a variantes no probadas.

## 3. Metas medibles iniciales

Son objetivos propuestos para negociar con la línea base, no rendimiento medido
ni requisitos de una norma. Si alguno no se alcanza, justificar cambio de diseño
o de requisito; no ocultarlo aumentando silenciosamente timeouts.

| Métrica | Meta inicial / forma de comprobar |
| --- | --- |
| UART bajo carga nominal | 0 overflow por carga propia en replay a baud/cadencia soportados |
| Drenaje UART | p99 del intervalo entre drenajes <25 ms con flujo continuo a 115200 baud; medir máximo aparte |
| Fix válido→display | p95 <200 ms desde fin de recepción hasta flush del dato; medir p99/máximo |
| Matching | p95 <200 ms con referencia de 1 Hz; cola no acumula fixes viejos |
| GPS vencido | Probar 3 s iniciales a 1 Hz; retirar indicación de actualidad sin esperar nuevos bytes |
| UI | 30 FPS objetivo durante animaciones compatibles, sin exigir refresco completo permanente |
| Heap interno | Meta ≥64 KiB mínimo con perfil combinado; registrar también mayor bloque libre |
| PSRAM | Meta ≥1 MiB no comprometido con perfil nominal; ajustable tras medir rodata y pools |
| Stack | Ningún overflow y margen observado revisado por tarea, incluyendo caminos de error |
| Revocación backup | Ningún nuevo envío tras revocación procesada; objetivo ≤250 ms para cerrar socket local |
| Activación de mapa | Objetivo barrera <250 ms; si no drena lectores, posponer sin forzar liberaciones |
| Flash de aplicación | Alerta CI a >85% del slot con imagen final firmada; rechazo absoluto si no cabe |
| Transferencias | No fijar MB/s antes del benchmark; priorizar memoria estable y operación local |
| Estabilidad | 24/72 h iniciales sin crecimiento sostenido de heap/colas ni resets no explicados |

El tiempo de reacción GPS→pantalla excluye la antigüedad con que el Ruptela
emitió su fix; reportar también edad de origen si se conoce. El cierre de socket
no retira paquetes ya enviados ni elimina la carrera celular/RS232 de IO418.

## 4. Matriz mínima de fallas

| Área | Casos obligatorios |
| --- | --- |
| Framing | Binario con NUL/newline/`$`, mensajes concatenados, corte en cada offset, CRC malo, colisiones, ruido largo, overflow y relock de baud |
| Estado | RMC inválido, campos ausentes/no finitos/fuera de rango, UTC que salta, GPS silencioso, IO ausente, IMEI que cambia |
| UI | Sin GPS/SD, límite desconocido/inferido/vencido, encendido/apagado durante flush, cambio de pantalla durante alerta, errores de touch |
| Mapas | Frío/caliente, bordes/aristas largas, colectoras, vía anónima, reset, truncamiento en cada campo, cambio de generación con lectores |
| Caché/SD | OOM, negativo por ausencia vs EIO, fragmentación, tarjeta lenta/extraída/cambiada, remount y límite de archivos |
| Red | SSID ausente, contraseña mala, DHCP/DNS lento, socket que no responde, desconexión, AP+STA y BLE bajo carga |
| Telemetría | 418 vuelve/vence en DNS/connect/send/ACK, ACK perdido/duplicado/NACK, cola llena, reinicio, backlog y cambio de tracker |
| Pairing | Cliente no autorizado/revocado, repetición de sesión, varios clientes, teléfono nuevo, pérdida de secreto y servicio de recuperación |
| Paquetes | Firma/hash malos, región/formato equivocados, path traversal, tamaño/offset overflow, chunks repetidos distintos, disco lleno |
| Persistencia | Corte antes/después de cada sync/checkpoint/selector; candidato/fallback dañados; estado RAM diferente de durable |
| OTA | Imagen para otra placa, slot insuficiente, bootloader sin rollback, B defectuosa, reset antes de confirmar, config/mapa incompatible al volver |
| Energía | Fuente débil, brownout, corte con SD/radio, ignición OFF durante trabajo, batería baja y reinicio de servicios |

Fixtures de protocolo deben incluir capturas/vectores independientes, no solo
paquetes producidos por el mismo encoder bajo prueba. Replay determinista prueba
consistencia; la exactitud de vía/límite necesita verdad de referencia.

## 5. Evidencia y herramientas existentes

Tests actuales del HUD, desde `esp/Firmware`:

```sh
make -C host_tests test
```

Ese arnés se creó en P00 (antes no existía, pese a estar citado aquí) y hoy
contiene la caracterización del lector/matcher previo a P04. Alcance y límites en
[host_tests/README.md](../host_tests/README.md); línea base en
[P00](baseline-P00.md).

Tests del piloto, desde raíz del repositorio:

```sh
make -C esp/starlink_pilot/host_tests test
```

El [runner](../../../tools/run_tests.sh) ejecuta Python, herramientas y tests IO
del HUD; la [CI](../../../.github/workflows/trazabilidad.yml) construye firmware.
Ampliar explícitamente con componentes compartidos, matcher, políticas e instalador.
No presentar los 205 tests Python reportados como cobertura del matcher C actual.

Corrección registrada en P00: ni `tools/run_tests.sh` ni
`.github/workflows/trazabilidad.yml` existían cuando se escribió esto. Ya se crearon
en la calificación integrada, con otro nombre para el workflow:

```sh
./tools/run_tests.sh              # todo lo verificable sin hardware
./tools/run_tests.sh --with-build # además compila el firmware
```

El workflow es [`.github/workflows/verificacion.yml`](../../../.github/workflows/verificacion.yml).
Cobertura real contra la matriz de fallas y límites en [P11](p11-calificacion.md).

Build HUD: entorno ESP-IDF 5.4.4 fijado y `idf.py build`. Análisis de tamaño:
`idf.py size` y `idf.py size-components` en ese entorno. Registrar binario final,
no confundir informe estático con heap dinámico.

Evidencia previa disponible y limitaciones: [estado actual](estado-actual.md).
Esta ampliación documental analizó el `.map` existente; no ejecutó pruebas de
hardware, flasheó, quemó eFuses ni envió al endpoint de producción. Las pruebas con
IMEI real requieren banco acordado para no competir con el tracker operativo.

## 6. Primer corte de implementación recomendado

Al aprobar ejecutar el plan, comenzar por P00 y un cambio pequeño de P02:
arranque degradado sin SD, callbacks no bloqueantes y estado de frescura probado.
Instrumentar P01 temprano para saber cuánto margen hay antes de integrar red.
Después P03/P04 con snapshots y matching extraído. El orden exacto de commits
puede adaptarse a dependencias, manteniendo siempre comparación con baseline.

Cada cambio debe indicar: responsabilidad extraída, comportamiento preservado o
modificado, pruebas ejecutadas, impacto de memoria/latencia y cómo volver a la
versión anterior. La primera meta no es «tener todas las funciones», sino que
agregar la siguiente no obligue a tocar UART, UI y mapas al mismo tiempo.
