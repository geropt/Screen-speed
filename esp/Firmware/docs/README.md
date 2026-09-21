# Firmware Waveshare ESP32-S3: documentación técnica

Revisión: 5 de septiembre de 2026. Investigación de hardware y SDK: 4 de septiembre.

El producto recibe NMEA e IO de un Ruptela, muestra velocidad actual y límite
obtenido de mapas locales, y debe evolucionar para enviar telemetría mediante
Wi-Fi/Starlink y actualizar mapas desde una app de teléfono.

Esta documentación distingue código existente, pruebas de laboratorio y diseño
propuesto. Las interfaces y los estados propuestos todavía no están implementados.
La revisión incluye los cambios locales presentes en el repositorio; no describe
necesariamente la versión instalada en cada unidad.

## Por dónde empezar

| Documento | Contenido |
| --- | --- |
| [Backlog ejecutable](tareas-firmware.md) | Checklist único: 56 tareas con dependencias y cierre; tiles actuales, v2/zonas opcional |
| [Blueprint del firmware definitivo](blueprint-firmware.md) | Documento principal: decisiones, módulos, UI, Ruptela, mapas, teléfono, OTA y recuperación |
| [Hardware, RAM y pantalla](hardware-y-memoria.md) | Placa 1.75, SRAM/PSRAM/GRAM, cálculos de buffers, presupuesto y experimentos de rendimiento |
| [Plan de implementación](plan-evolucion.md) | P00–P11 con dependencias, tareas concretas, metas y pruebas de aceptación |
| [Línea base P00](baseline-P00.md) | Dossier ejecutado: commit, toolchain, build limpio con hashes, dataset, defaults corregidos y pendientes de hardware |
| [Recursos P01](p01-recursos.md) | Extracción de servicios de placa, instrumentación, filas de buffer configurables y comparación estática medida |
| [Calificación integrada P11](p11-calificacion.md) | Runner y CI, cobertura contra la matriz de fallas, y el informe de qué NO está verificado |
| [Pairing, OTA y seguridad P08–P10](p08-p10-seguridad.md) | Contratos de vinculación y capacidades, política de OTA A/B, threat model y lista de lo no verificado |
| [Cápsula P07](p07-capsula.md) | Contenedor por rangos con equivalencia probada, instalador transaccional e inyección de fallas |
| [Conectividad P05](p05-conectividad.md) | Outbox con confirmación separada, política de permiso con revocación y dueño único de la radio |
| [Mapas P04](p04-mapas.md) | Matcher extraído con fuente inyectable, map_store con generación y replay de equivalencia contra el golden de P00 |
| [UI P03](p03-ui.md) | Presenter único escritor, estados de límite y velocidad, alerta con histéresis y traza fix→flush |
| [Estado P02b](p02b-estado.md) | Estado del vehículo con edad por señal, tracker_epoch, mailbox de fixes y arranque sin tarjeta |
| [Framing P02a](p02a-framing.md) | Componente compartido del Ruptela, framer NMEA binario-seguro y publicación sólo tras validación |
| [Estado actual](estado-actual.md) | Hardware configurado, módulos existentes, problemas y evidencia de pruebas |
| [Arquitectura resumida](arquitectura.md) | Vista breve de responsabilidades, tareas, contratos y reglas para agregar funciones |
| [Wi-Fi y Starlink](starlink-backup.md) | Reutilización del piloto, IO 418, entrega de paquetes y convivencia con la app |
| [App y mapas](actualizacion-mapas.md) | Emparejamiento, transferencia, validación, activación y recuperación |
| [Cápsula de mapa](capsula-mapas.md) | Propuesta de actualización con un botón y contenedor consultable sin archivos por tile |
| [Procesamiento de mapas](procesamiento-mapas.md) | Qué contiene un tile, reparto PC/app/HUD y medición de los 1.961 tiles locales |

Para ejecutar: backlog → detalle del plan/blueprint según la tarea. Para entender
el diseño: blueprint → hardware/memoria → plan. El blueprint amplía
la propuesta inicial; cuando detalla una decisión que antes estaba abierta,
prevalece como referencia de diseño. Sigue sin ser una implementación aplicada.

La regla central de la propuesta: **la conectividad y las transferencias no deben
bloquear la recepción del Ruptela ni la actualización de velocidad en pantalla**.
Una función nueva necesita un responsable de su estado, límites de recursos,
comportamiento ante fallas y pruebas antes de incorporarse al producto.

## Relación con documentos existentes

- [Piloto Starlink](../../starlink_pilot/README.md): firmware de banco separado;
  sus parsers y protocolo son candidatos a componentes compartidos.
- [Notas originales de Starlink](../../../starlink-backup.md): capturas y replay
  contra el servidor. Algunas tareas allí pendientes ya existen en el piloto.
- [Diseño de formato v2](../../../.kiro/specs/tile-format-v2/design.md) y
  [tareas v2](../../../.kiro/specs/tile-format-v2/tasks.md): borrador de investigación
  no aprobado. No es una migración comprometida ni requisito para app, mapas u OTA.
- [Notas de secure boot](secure-boot-notes.md): investigación previa sobre
  protección del dispositivo; no constituye una configuración aplicada ni una
  validación de producción.

## Cómo mantener esta documentación

Al integrar una función, actualizar su estado y registrar qué prueba se ejecutó,
en qué placa y con qué firmware/dataset. Separar cambios de arquitectura de cambios
de algoritmo: una extracción de código debe preservar su resultado; una mejora
de matching debe explicar y probar sus diferencias.

Un build correcto, un ACK de laboratorio y un ensayo completo de conducción son
evidencias distintas. No presentar una como reemplazo de las otras.
