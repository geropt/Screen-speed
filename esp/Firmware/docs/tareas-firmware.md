# Backlog ejecutable del firmware

Estado: plan pendiente. Decisión de alcance: conservar geometría y función principal;
v2/zonas se evalúa después sin bloquear ninguna integración. Se propone una
[cápsula indexada](capsula-mapas.md) para actualizar sin filesystem de tiles.
Esta lista no autoriza
flasheos, conexiones a producción ni cambios irreversibles de seguridad.

## 1. Cómo vamos a trabajar

Este documento es el **único checklist de ejecución general**. El
[blueprint](blueprint-firmware.md) explica el diseño y el
[plan por etapas](plan-evolucion.md) desarrolla pruebas y metas. No marcar tareas
en tres documentos distintos. Los borradores antiguos de v2 no son un backlog aprobado.

- `[ ]`: pendiente. Marcar `[x]` solo con evidencia de cierre. Si está en curso o
  bloqueada, agregar responsable, motivo y siguiente paso debajo de la tarea.
- `Dep.` indica tareas que deben cerrar primero; no implica que necesitemos
  completar todo el firmware para probar una pieza.
- Cada tarea busca una entrega revisable. Si crece demasiado, dividirla en
  subtareas con el mismo prefijo antes de mezclar responsabilidades.
- Un movimiento de archivos preserva comportamiento. Una corrección funcional
  lleva su propio test/diferencia. No mezclar ambas con upgrade de librerías.
- Se puede reutilizar código existente para cerrar una tarea: no hay que volver
  a escribirlo. Primero comprobar qué ya cumple el contrato y qué le falta.
- No asignar plazos ficticios. Estimar después de T01–T06 y revisar por entrega.

### Cierre común de una tarea

Código o documento revisable, tests aplicables ejecutados, diferencias esperadas
registradas y documentación actualizada cuando cambia un contrato. Los cambios
de runtime registran impacto de memoria/latencia y recuperación. Si necesita
hardware, un test host no lo sustituye: queda pendiente esa validación.

Formato de evidencia sugerido:

```text
Txx | cambio/commit | build y dataset | prueba y resultado
hardware/CFG si corresponde | memoria/latencia si corresponde
limitaciones conocidas | recuperación | responsable/fecha
```

## 2. Reutilizar antes de construir

| Área | Base que se conserva | Qué agregamos o corregimos |
| --- | --- | --- |
| Pantalla | Puerto/driver funcional, LVGL y proyecto EEZ | Ownership, presenter, estados y buffers medidos |
| GPS/IO | Parsers HUD y piloto, capturas y tests existentes | Unificación, framing por longitud, frescura y límites |
| Mapas | Tiles, lector, caché y generador actuales | Interfaz de fuente, reset, errores y paquetes versionados |
| Starlink | Protocolo, agrupación y experiencia del piloto | Gestor Wi-Fi único, permiso cancelable y retención hasta ACK |
| Tareas/colas | FreeRTOS del SDK | Propiedad y presupuestos explícitos, no un framework nuevo |
| Red/seguridad | APIs de ESP-IDF y primitivas criptográficas mantenidas | Configuración, política y adaptación del producto |
| OTA | Slots existentes y APIs OTA de ESP-IDF | Verificación, autoprueba, rollback y compatibilidad |
| Identidad/CI | Herramientas de build/inventario, runner y CI actuales | Completar cobertura y evidencia reproducible |

No construir un event bus universal, scheduler, parser alternativo completo,
cifrado propio ni sistema de plugins. No crear todas las carpetas del blueprint
de entrada: extraer componentes cuando una frontera o reutilización concreta lo
justifique. No migrar LVGL/IDF/BSP como requisito previo para ordenar el código.

## 3. Entregas y dependencias

| Entrega | Tareas | Resultado usable | Etapas de referencia |
| --- | --- | --- | --- |
| A. Referencia y medición | T01–T06 | Sabemos qué funciona y cuánto consume | P00/P01 |
| B. Núcleo de recepción | T07–T13 | Ruptela/estado robustos sin depender de SD | P02 |
| C. Pantalla ordenada | T14–T20 | UI centralizada, velocidad independiente | P01/P03 |
| D. Tiles encapsulados | T21–T25 | Mismo mapa y matching con ciclo de vida explícito | P04 |
| E. Comunicaciones | T26–T32 | Wi-Fi/Starlink integrados y entrega comprobable | P05 |
| F. Actualización de tiles | T33–T39 | Instalación recuperable con cliente de banco | P07 |
| G. Teléfono | T40–T45 | Pairing, configuración y transferencia | P08 |
| H. OTA | T46–T51 | Firmware actualizable con rollback | P09 |
| I. Producción | T52–T56 | Provisión, calificación y soporte | P10/P11 |

Orden inicial: A → B → C/D. Luego E y F según sus dependencias; G usa ambas.
H puede probarse con cliente de banco antes de terminar G. P06 (mapas v2/zonas)
no está en este camino. La seguridad de protocolos se define antes de implementar
transferencias; I cierra la provisión y calificación de producción.

## 4. Checklist

### A — Referencia y medición

- [ ] **T01 — Identificar el estado de código actual.** Dep.: ninguna.
  Registrar commit, cambios locales, build ID y dependencias; separar lo instalado
  de lo presente en el árbol. Cierre: referencia reproducible sin descartar cambios.
- [ ] **T02 — Capturar baseline funcional.** Dep.: T01.
  Reutilizar tests/replays y dataset actuales. Guardar resultados de GPS, mapa,
  ignición y UI. Cierre: corpus identificable para comparar cada extracción.
- [ ] **T03 — Completar dossier de hardware y Ruptela.** Dep.: T01.
  SKU/revisión, panel, flash/PSRAM, SD, alimentación, conversor y modelo/FW/CFG del
  tracker. Cierre: confirmados cableado RX18, cadencias y variantes; incógnitas anotadas.
- [ ] **T04 — Normalizar build y configuración.** Dep.: T01, T03.
  Resolver defaults/flash, fuentes duplicadas y dependencias CMake sin upgrade.
  Cierre: build desde directorio limpio con perfil correcto y comparación con baseline.
- [ ] **T05 — Agregar mediciones mínimas.** Dep.: T04.
  Heap por capacidades, bloque mayor, stacks, colas, UART, match y flush; reutilizar
  diagnóstico/build info. Cierre: informe comparable, sin logging que distorsione la carga.
- [ ] **T06 — Fijar presupuesto y protocolo de banco.** Dep.: T02, T05.
  Medir HUD frío/caliente y acordar umbrales iniciales del plan; registrar tarjetas,
  replay y duración. Cierre: referencia cuantitativa, no cifras deducidas solo del linker.

### B — Recepción, estado y recuperación

- [ ] **T07 — Arrancar sin SD.** Dep.: T02.
  Desacoplar inicio UART/ignición de montaje; manejar retornos de tareas/colas.
  Cierre: SD ausente no impide recibir velocidad ni controlar ignición.
- [ ] **T08 — Retirar trabajo bloqueante de callbacks.** Dep.: T07.
  Mover display/inventario/persistencia a sus consumidores y corregir API de cola
  desde tarea. Cierre: callbacks acotados y saturación contabilizada.
- [ ] **T09 — Ampliar fixtures de flujo mixto.** Dep.: T02.
  Partir de los tests IO actuales y capturas autorizadas: cortes, NUL, newline,
  ruido, checksums y falsos positivos. Cierre: reproducen las limitaciones actuales.
- [ ] **T10 — Corregir framing y validación incremental.** Dep.: T09.
  Reutilizar lo válido de ambos parsers, drenar UART_DATA y procesar por longitud.
  Cierre: solo publicar mensajes completos válidos y resincronizar tras pérdida.
- [ ] **T11 — Compartir parser entre HUD y piloto.** Dep.: T10.
  Extraer una implementación con adaptadores de UART/pines; no mantener copias.
  Cierre: ambos targets compilan y pasan los mismos vectores de protocolo aplicables.
- [ ] **T12 — Introducir snapshot de vehículo y frescura.** Dep.: T10.
  Presencia/validez, reloj monotónico, edad propia de IO409/418 e identidad/epoch.
  Cierre: silencio, RMC inválido o cambio de tracker no conservan permisos válidos.
- [ ] **T13 — Separar último fix y records.** Dep.: T08, T11, T12, T05.
  Mailbox para pantalla/matching y pool/cola de records con límites; ensayar ring
  UART. Cierre: un consumidor lento no acumula velocidad obsoleta ni bloquea recepción.

### C — UI y memoria de pantalla

- [ ] **T14 — Fijar regeneración EEZ.** Dep.: T02.
  Registrar editor/exportación y correspondencia del proyecto con `main/ui`.
  Cierre: regenerar no pierde lógica manual ni altera inadvertidamente el diseño.
- [ ] **T15 — Encapsular puerto y recursos de display.** Dep.: T03, T04.
  Concentrar pines, lifecycle, errores y contrato de flush conservando driver.
  Cierre: verificar discrepancia CO5300/SH8601 sin reemplazo injustificado.
- [ ] **T16 — Comparar tamaños de buffers.** Dep.: T05, T06, T15.
  Ensayar 116/64/48/32 filas con mismos assets y reloj; medir calidad, latencia y
  heap. Cierre: configuración elegida con evidencia, no ahorro supuesto.
- [ ] **T17 — Centralizar el escritor LVGL.** Dep.: T08, T12, T14, T15.
  Migrar `vars`, `dynamic`, splash y transiciones a modelo/presenter/puerto;
  conservar callback de finalización de flush. Cierre: workers no tocan widgets.
- [ ] **T18 — Independizar el velocímetro de SD.** Dep.: T13, T17.
  Publicar velocidad desde snapshot; aplicar resultado de mapa luego con fix y
  generación. Cierre: SD lenta no congela velocidad ni aplica límites de otro fix admitido.
- [ ] **T19 — Extraer alertas y estados visuales.** Dep.: T17, T18.
  Lógica pura de exceso/frescura y estados desconocido/inferido/último conocido;
  retirar límite inicial ficticio. Cierre: tests y comparación visual del HUD normal.
- [ ] **T20 — Agregar navegación mínima de servicio.** Dep.: T15, T19.
  Estado, diagnóstico y acciones de mantenimiento; integrar touch/bus compartido
  si se usa. Cierre: las pantallas emiten intenciones, no ejecutan SD/red directamente.

### D — Ordenar tiles actuales, sin migrar formato

- [ ] **T21 — Extraer matcher conservando resultados.** Dep.: T02.
  Reutilizar matemática C y añadir estado/reset e interfaz mínima de candidatos.
  Cierre: replay antes/después equivalente; ninguna heurística nueva en la extracción.
- [ ] **T22 — Encapsular fuente y generación.** Dep.: T12, T21.
  `map_store` con tiles actuales, ownership y resultados etiquetados por fix/mapa.
  Extraer fuente de bytes por celda con backend de directorio; cápsula llega en F.
  Cierre: reset explícito y rechazo de resultados de una generación desactivada.
- [ ] **T23 — Corregir caché y errores de lectura.** Dep.: T22, T05.
  Ausencia vs error SD, negativos, OOM/expulsión y archivo truncado; conservar tests
  independientes de cada corrección. Cierre: no usar prefijos corruptos ni límites viejos silenciosos.
- [ ] **T24 — Coordinar acceso y recuperación de SD.** Dep.: T07, T22.
  Un montaje/remount, lectores activos, límites de archivos y operaciones acotadas.
  Cierre: extracción/cambio/error no desmonta recursos en uso ni reinicia todo el HUD.
- [ ] **T25 — Aceptar núcleo offline ordenado.** Dep.: T16, T18, T19, T23, T24.
  Replay, bordes, vías anónimas, caché fría/llena y fallas SD en Waveshare.
  Cierre: funcionamiento normal preservado, correcciones explicadas y presupuesto cumplido.

### E — Wi-Fi y Starlink

- [ ] **T26 — Compartir protocolo de salida del piloto.** Dep.: T11, T02.
  Reutilizar encoder/decoder y tests de cmd68/heartbeat/ACK. Fijar límites y revisión
  de protocolo/backend. Cierre: vectores válidos sin reinterpretar records desde NMEA.
- [ ] **T27 — Definir y probar permiso de backup.** Dep.: T12, T26, T03.
  Frescura, dwell, ignición e identidad con reloj falso. Cierre: IO418 no se
  interpreta como ACK/exclusión TCP y toda revocación tiene una prueba.
- [ ] **T28 — Crear dueño único de conectividad.** Dep.: T04, T05.
  Reutilizar Wi-Fi del piloto y SDK; redes persistentes, solicitudes y reconexión.
  Cierre: consumidores no paran la radio de otros ni borran globalmente NVS.
- [ ] **T29 — Implementar outbox y lote en vuelo.** Dep.: T26, T27.
  Retener hasta ACK; definir límites, antigüedad, duplicados y pérdida al apagar.
  Cierre: ACK perdido/NACK/cola llena tienen resultado explícito. Journal si el
  requisito es persistencia; no prometerla con cola RAM.
- [ ] **T30 — Integrar sesión cancelable.** Dep.: T28, T29.
  Adaptar socket del piloto, timeouts y revalidación antes de envíos; no bloquear
  revocación en DNS/connect/ACK. Cierre: retorno/vencimiento de 418 probado en cada espera.
- [ ] **T31 — Cerrar transporte seguro de producción.** Dep.: T26, T28.
  Confirmar endpoint/TLS, certificados y bootstrap de hora con APIs existentes.
  Cierre: no desactivar validación ni asumir que TCP del piloto está cifrado.
- [ ] **T32 — Validar Starlink con HUD completo.** Dep.: T25, T30, T31.
  Servidor de banco, red intermitente, ACKs y carga SD/UI. Cierre: métricas y
  cancelación aceptadas, sin competir con el IMEI del tracker en producción.

### F — Actualización con cápsulas y geometría actual

- [ ] **T33 — Definir paquete e inventario autenticado.** Dep.: T22, T24.
  Definir cápsula, índice paginado coordenadas→offset/longitud, tamaños/hashes,
  parámetros de lector, versión, origen y firma; límites de memoria y autenticación
  de páginas. Cierre: contrato con vectores; no nuevo formato geográfico.
- [ ] **T34 — Empaquetar desde herramientas actuales.** Dep.: T33.
  Agregar empaquetador/verificador y backend de cápsula sobre la fuente de T22,
  sin rehacer extracción OSM ni extraer archivos por tile en el HUD.
  Cierre: mismos bytes y replay que directorio, memoria acotada, manifiesto
  determinista y sin modificar headers C.
- [ ] **T35 — Implementar recepción acotada.** Dep.: T33, T24.
  Begin/write/status/finish/cancel, offsets idempotentes y progreso durable;
  rutas internas y cuota de espacio. Cierre: no sobrescribe activo ni carga paquete entero.
- [ ] **T36 — Verificar candidata completa.** Dep.: T34, T35, T23.
  Firma, índice, cada payload y parámetros soportados; abrir con backend de cápsula
  y parser actual. Cierre: truncamiento, claves duplicadas, rangos inválidos,
  corrupción o incompatibilidad impiden READY.
- [ ] **T37 — Activar y recuperar generaciones.** Dep.: T22, T36.
  Barrera de lectores, selección recuperable, invalidación/reset y fallback.
  Cierre: corte en cada transición no selecciona contenido incompleto; velocidad sigue independiente.
- [ ] **T38 — Probar fallos persistentes y limpieza.** Dep.: T37.
  Cortes físicos, SD cambiada/llena/corrupta, reanudación y generaciones protegidas.
  Cierre: limpiar solo inactivos elegibles; no prometer inmunidad física de FAT.
- [ ] **T39 — Cerrar instalador con cliente de banco.** Dep.: T25, T38.
  Reutilizar herramientas de host para ejercitar contrato antes de desarrollar app.
  Cierre: recibir/verificar/activar/cancelar/reanudar demostrados con cápsula v1.

### G — Conexión con teléfono

- [ ] **T40 — Cerrar UX y contrato de pairing.** Dep.: T20, T33.
  Definir teléfonos objetivo, dueño/servicio, posesión única, revocación y
  recuperación. BLE como entrada habitual y solicitud de mantenimiento desde app
  vinculada. Cierre: flujos acordados sin configurar Wi-Fi del HUD ni usar MAC/IMEI
  como contraseña; mecanismo de primera posesión definido con touch deshabilitado.
- [ ] **T41 — Integrar provisioning mantenido.** Dep.: T28, T40.
  Evaluar y adaptar provisioning/protocomm del SDK fijado; no crear criptografía.
  Cierre: sesión autorizada y ciclo de vida de radio/memoria que permite volver a emparejar.
- [ ] **T42 — Exponer API versionada y bulk seguro.** Dep.: T35, T41.
  Capacidades, configuración y actualización por BLE GATT con fragmentación,
  créditos y ACK durable; Wi-Fi opcional ligado a la misma autorización.
  Cierre: upload autenticado, buffers acotados, un escritor y reanudación comprobada
  entre enlaces anunciados; clientes incompatibles reciben error explícito.
- [ ] **T43 — Implementar flujo de app.** Dep.: T39, T40, T42.
  Descargar paquete al teléfono y transferirlo por BLE; ofrecer modo rápido Wi-Fi
  asistido según duración estimada, con opción de seguir por BLE si se rechaza.
  Distinguir recepción, verificación y activación. Cierre: cápsula representativa
  instalada sin configurar Wi-Fi en el HUD ni Internet durante transferencia.
- [ ] **T44 — Coordinar mantenimiento y telemetría.** Dep.: T32, T39, T42.
  Un controlador decide pausa, energía, detenido y exclusión de operaciones.
  Cierre: prioridad explícita y progreso conservado; GPS inválido no equivale a detenido.
- [ ] **T45 — Validar teléfonos y sesiones reales.** Dep.: T43, T44.
  Background, cambio de red, corte, reanudación, revocación y cliente antiguo en
  plataformas acordadas; BLE solo y transición SoftAP sin exigir BLE permanente.
  Cierre: duración por tamaño, convivencia con UART/UI/telemetría, pruebas end-to-end
  y memoria sin fugas; suspensión de app conserva progreso recuperable.

### H — OTA de firmware

- [ ] **T46 — Fijar contrato y base OTA.** Dep.: T03, T04, T33.
  Reusar slots/API SDK; revisar tamaño firmado, bootloader instalado, firma y
  compatibilidad. Cierre: decisión para unidades existentes/nuevas; no reparticionado implícito.
- [ ] **T47 — Implementar streaming a slot inactivo.** Dep.: T28, T46.
  Adaptar OTA del SDK con cliente HTTPS o de banco; límites, cancelación y
  verificación. Cierre: imagen mala/incompatible no altera aplicación activa.
- [ ] **T48 — Implementar autoprueba y rollback.** Dep.: T47, T25.
  Confirmar imagen solo después de salud interna; SD/Internet/GPS ausentes son
  casos tolerables. Cierre: imagen defectuosa vuelve a la anterior en bootloader real.
- [ ] **T49 — Preservar configuración y mapas al volver.** Dep.: T37, T48.
  Schemas versionados, generaciones compatibles y limpieza protegida.
  Cierre: aplicación anterior arranca con configuración/mapa legibles tras rollback.
- [ ] **T50 — Integrar política y UI de OTA.** Dep.: T20, T47, T48.
  Un mantenimiento compartido con mapas, energía y autorización; usar contrato
  de T44 si ya existe, no otro controlador. Cierre: mapa y OTA no mutan simultáneamente.
- [ ] **T51 — Validar OTA con cortes y carga UART.** Dep.: T49, T50, T05.
  Cortar en erase/write/selección/primer boot; medir caché/PSRAM/watchdog/UART.
  Cierre: recuperación documentada, sin prometer OTA en conducción ni resume no implementado.

### I — Producción y mantenimiento del producto

- [ ] **T52 — Cerrar provisión y seguridad.** Dep.: T31, T41, T46, T51.
  Custodia/rotación de claves, credenciales por unidad, Secure Boot/cifrado y
  recuperación en unidades de ensayo. Cierre: proceso aprobado antes de operaciones irreversibles.
- [ ] **T53 — Completar CI y checklist de release.** Dep.: T25, T32, T39, T51.
  Extender runner/CI existentes con componentes compartidos, límites de imagen
  firmada y contratos. Cierre: artefactos trazables; hardware pendiente no aparece como aprobado.
- [ ] **T54 — Calificar integración sostenida.** Dep.: T45, T51, T52, T53.
  Carga combinada, red inestable, ciclos de servicio/energía y soak del plan.
  Cierre: memoria/latencia/colas y errores dentro de límites, sin fugas sostenidas.
- [ ] **T55 — Validar campo controlado.** Dep.: T54.
  Rutas de referencia, visibilidad, alimentación, temperaturas y failover real.
  Cierre: resultados atribuibles a hardware/build/dataset; limitaciones declaradas.
- [ ] **T56 — Entregar recuperación y soporte.** Dep.: T55.
  Manual de provisión/actualización/rollback, exportación sanitizada y diagnóstico
  sin mapas/red. Cierre: procedimiento ejecutado por alguien que no escribió el cambio.

## 5. Fuera del camino obligatorio

No iniciar por anticipación. Estas tareas requieren prioridad explícita y no
bloquean las entregas anteriores:

- [ ] **R01 — Evaluar mapas agrupados/zonas.** Base: T25 y mediciones reales.
  Revisar críticamente v2 frente a geometría actual en cápsula; no confundir
  contenedor de entrega con migración geográfica. Entregar comparación y decisión;
  conservar geometría actual es un resultado válido. Ninguna integración depende de R01.
- [ ] **R02 — Optimizar más allá de buffers pequeños.** Base: T16/T54.
  PSRAM con staging, caché mayor o XIP solo si un perfil demuestra necesidad.
  No cambiar todos los parámetros para perseguir una cifra de memoria libre.
- [ ] **R03 — Evaluar upgrade IDF/LVGL/BSP.** Base: baseline y tests estables.
  Motivo concreto, compatibilidad y benchmark en rama aislada; no sustituir el
  driver funcional por uno más nuevo solo por disponibilidad.
- [ ] **R04 — Incorporar periféricos adicionales.** Base: caso de uso aprobado.
  IMU, audio, RTC y ahorro energético por incrementos. Touch/energía necesarios
  para mantenimiento sí pertenecen al alcance principal; micrófonos/voz no.
- [ ] **R05 — Optimizar distribución de mapas.** Base: T39/T45 y tamaños medidos.
  Diferenciales, varias regiones atómicas o descarga autónoma desde servidor
  reutilizan el instalador. No crear un segundo camino de activación.

## 6. Primer bloque para ejecutar

Empezar por T01/T02 y recopilar T03; luego T04–T06. Mientras se completa la
información física, T07–T10 pueden avanzar con fixtures sin asumir otro hardware.
La primera entrega funcional debe demostrar arranque sin SD y recepción acotada.
Después cerrar UI/tiles y probar el núcleo offline antes de integrar servicios.

No comenzar por mover todo a veinte componentes ni implementar v2. El progreso
se mide por comportamiento comprobado, responsabilidades claras y facilidad para
agregar la próxima función sin alterar las que ya funcionan.
