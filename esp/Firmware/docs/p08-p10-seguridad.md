> Alcance de esta etapa: **contratos, lógica pura y diseño**. No hay BLE, no hay
> flasheo, no hay eFuses. Lo que se puede probar sin hardware está probado; el resto
> está listado como pendiente, con nombre y motivo.

# P08–P10 — Pairing, OTA A/B y seguridad

Revisión: 2026-09-05. **Ninguna de las tres etapas queda cerrada.** P08 necesita
teléfonos reales, P09 necesita la placa con su bootloader, y P10 necesita unidades de
ensayo donde se puedan quemar eFuses.

## 1. Qué se implementó y qué no

| Pieza | Estado | Probado en host |
| --- | --- | --- |
| Negociación de `api_version` y capacidades | Implementada | Sí |
| Máquina de pairing: ventana, intentos, posesión única, revocación | Implementada | Sí, 15 casos |
| Sesión bulk atada a la vinculación | Implementada | Sí |
| Reanudación por offset durable | Ya estaba en P07 (`map_installer`) | Sí, 20 casos |
| Política de OTA: verificación, autoprueba, anti-rollback | Implementada | Sí, 19 casos |
| Transporte BLE | **No** | — |
| Transferencia bulk real | **No** | — |
| Flasheo de slots y selección de boot | **No** | — |
| Secure Boot, flash encryption, eFuses | **No** | — |
| Custodia real de claves | **No** | — |

## 2. Pairing: lo que el diseño hace estructural

La regla que el plan pide explícitamente: **no existe un canal que acepte bytes «porque
BLE ya autenticó otro canal»**. Cada sesión bulk lleva el id de la vinculación y un
token propio, y `pairing_check_session()` es la comprobación que el transporte tiene
que hacer en cada mensaje. Sin eso, el error clásico de este diseño es exponer una carga
HTTP abierta en la red local porque el pairing ocurrió por otro lado.

| Regla | Por qué |
| --- | --- |
| Posesión única | Dos dueños simultáneos no tienen forma de resolverse |
| La ventana se abre con acción física | Es la única prueba de posesión que un HUD sin teclado puede dar |
| Ventana temporal | Una ventana siempre abierta convierte a cualquier vecino de radio en candidato |
| Límite de intentos | La ventana temporal sola no impide probar secretos en serie |
| Reabrir la ventana desbloquea | Es lo que permite recuperar el control tras perder el teléfono |
| Revocación inmediata | Esperar un vencimiento deja al teléfono perdido operando mientras tanto |
| Capacidades explícitas | Un cliente no puede usar lo que el dispositivo no declaró |

`BULK_WIFI` y `RESUME_SWITCH` **no se declaran**, porque no hay implementación detrás.
Declarar una capacidad inexistente haría que el cliente la use y falle; es la misma
regla que el plan aplica a la UI, «una función no disponible debe aparecer
deshabilitada o no aparecer».

### Lo que NO es criptografía

El token de sesión se deriva del id y del secreto con XOR. **No es criptografía**: es la
ligadura entre sesión y vinculación. Está escrito en el código, no sólo acá. Derivarlo
con un MAC de verdad, y guardar el secreto donde no se pueda leer, es P10.

La comparación de secretos sí es en tiempo constante: con un `memcmp` común el tiempo de
respuesta filtra cuántos bytes coincidieron, y eso no cuesta nada evitarlo.

## 3. OTA A/B: el orden de verificación importa

| Orden | Comprobación | Por qué en ese lugar |
| --- | --- | --- |
| 1 | Modelo | Una imagen de otra placa es el error más caro: deja el equipo sin arrancar |
| 2 | Tamaño | Barato, y sin esto el resto es inútil |
| 3 | Anti-rollback | No tiene sentido verificar el contenido de algo que no se va a instalar por su versión |
| 4 | Misma versión | Evita una actualización sin efecto con el riesgo de un reinicio |
| 5 | Hash | Integridad del contenido |
| 6 | Firma | Autenticidad, cuando se exige |
| 7 | Compatibilidad NVS y mapas | Si la nueva no puede leer lo instalado, volver atrás tampoco recuperaría el equipo |
| 8 | Energía | Es la condición más volátil; no conviene rechazar por batería algo que además era inválido |

### Autoprueba acotada de verdad

El plan lo pide con estas palabras: «ausencia de Internet/GPS/SD no equivale a imagen
defectuosa». Las tres se ignoran explícitamente, con el motivo en el código: el HUD tiene
que funcionar sin ninguna, y tomarlas como falla produciría rollbacks por estar en un
estacionamiento subterráneo.

Lo que sí cuenta como falla de imagen: pánico o watchdog, fallo del panel, no poder crear
tareas, no poder abrir el UART.

**Decisión discutible, anotada como tal**: se exige que la pantalla haya mostrado
velocidad al menos una vez dentro de la ventana de 60 s. En una unidad instalada sin
tracker conectado eso provocaría un rollback innecesario. Si resulta un caso real, hay que
exigir además haber visto bytes en el UART. Preferí dejarlo explícito antes que elegir el
criterio laxo en silencio.

### El anti-rollback y el fallback son la misma decisión

`ota_policy_fallback_allowed()` usa el mismo umbral que rechaza instalar. Subir el mínimo
protege de reinstalar una versión vulnerable **y** deja sin fallback a las unidades cuya
imagen anterior queda por debajo. Hay una prueba de las dos caras, porque verlas separadas
es cómo se llega a una flota sin camino de vuelta.

## 4. Threat model práctico

Los seis escenarios que el plan nombra, con lo que hay hoy y lo que falta.

| Amenaza | Qué la limita hoy | Qué falta |
| --- | --- | --- |
| **Vecino de radio** | Ventana de pairing acotada y con acción física; límite de intentos; capacidades explícitas | El transporte BLE con cifrado y su emparejamiento; hoy no hay radio implementada |
| **Teléfono revocado o perdido** | Revocación inmediata que invalida la sesión en curso; reabrir la ventana desde el dispositivo recupera el control | Persistir la revocación en NVS y que sobreviva a un reinicio |
| **SD manipulada** | La cápsula verifica índice y payloads antes de activarse; una candidata inválida no se activa; sin mapas la velocidad sigue funcionando | **CRC-32 detecta corrupción, no manipulación.** Cualquiera con la tarjeta puede editar el contenido y recalcular los CRC. Hace falta la firma del manifiesto |
| **Servidor o paquete falso** | La política de OTA exige firma por defecto; la lista de comandos a descartar rechaza CFG/FOTA/SMS/SET_IO | Verificar la firma contra una clave; hoy `signature_valid` lo decide el llamador. Y definir TLS y endpoint, pendiente de P05 |
| **Acceso físico** | Nada | Secure Boot, flash encryption y NVS cifrado. Sin eso, con acceso físico se lee y se reemplaza todo |
| **Filtración de credenciales** | No hay credenciales de build en el producto (el piloto sí las tiene, y el plan lo prohíbe para producción); no hay borrado global de NVS | Identidad por unidad, rotación y un procedimiento de provisión |

### Custodia de claves: lo que hay que decidir antes de escribir código

Ninguna de estas decisiones está tomada, y tomarlas mal es difícil de revertir:

1. **Clave de firmware.** Quién la tiene, en qué HSM o repositorio cifrado, quién puede
   firmar una release y cómo se revoca si se filtra. **La clave privada nunca va al
   dispositivo ni al repositorio.**
2. **Clave de mapas.** Puede ser distinta de la de firmware: firmar cobertura es una
   operación más frecuente y con menos riesgo, así que compartir la clave del firmware
   sube el riesgo sin necesidad.
3. **Identidad por unidad.** Un par de claves por dispositivo, generado en provisión, con
   la privada en el propio equipo y la pública registrada. Sin identidad por unidad, un
   secreto filtrado de una unidad sirve para todas.
4. **Credenciales de pairing.** Derivadas de la identidad de la unidad, no compartidas
   entre unidades.
5. **Anti-rollback.** Quién decide subir el mínimo, y con qué evidencia de que ninguna
   unidad en campo quedará sin fallback.
6. **Diagnóstico sanitizado.** Qué se puede loguear. Hoy los logs incluyen el IMEI, que
   identifica al vehículo.

## 5. Verificación ejecutada

| Qué | Resultado |
| --- | --- |
| `test_pairing` | 15 casos con ASan/UBSan, en verde |
| `test_ota_policy` | 19 casos, en verde |
| Build del HUD | Compila; los dos componentes se construyen |
| Las 13 suites de host | En verde |

## 6. Lo que queda sin verificar, con nombre

Esta lista es el entregable más honesto de la etapa.

**Necesita teléfonos reales (P08):**
- Transporte BLE completo, con su emparejamiento y cifrado.
- Fragmentación y créditos acotados para la transferencia bulk.
- App en background, teléfono bloqueado, cambio de red a mitad de transferencia.
- SoftAP sin Internet, y AP que no permite tráfico entre clientes.
- Cliente de versión anterior contra firmware nuevo.
- Reanudación cambiando de transporte, que hoy **no se declara como capacidad** porque no
  existe.

**Necesita la placa y su bootloader (P09):**
- Que el bootloader instalado realmente tenga rollback. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
  está **apagado** en la configuración actual: quedó anotado desde P00 y sigue así.
- Tamaño final de la imagen firmada, que hoy ocupa 53,5 % del slot sin firma.
- Cortes durante descarga, borrado, escritura, selección y primer boot.
- Pausas de flash contra UART, watchdog y PSRAM.

**Necesita unidades de ensayo (P10):**
- Secure Boot y flash encryption, con su recuperación ensayada. **No se queman eFuses en
  la unidad de desarrollo sin aprobación**, y el plan lo prohíbe explícitamente.
- Provisión repetible y verificable.
- Revisión de operaciones irreversibles antes de la primera unidad de producción.

**Necesita banco con IMEI propio:**
- Todo el camino de telemetría, ya anotado en P05.
