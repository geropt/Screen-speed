# Cápsula de mapa: actualizar cobertura sin administrar tiles

Estado: propuesta de diseño, 2026-09-05; pendiente de implementación y mediciones.
Responde a la decisión de evaluar v2/zonas después, sin bloquear integraciones.
La cápsula cambia el almacenamiento y la entrega; conserva la geometría v1.
El [análisis del procesamiento](procesamiento-mapas.md) explica los datos actuales
y registra una medición de la copia local: 1.961 tiles y 3.028.397 bytes de payload.

## Experiencia del cliente

El cliente elige una cobertura con nombre, por ejemplo «Buenos Aires», y toca
**Actualizar mapas**. La app recuerda esa elección, consulta la versión instalada
y descarga la edición compatible cuando tiene Internet. Cerca del HUD, la app
lo reconoce por BLE y puede transferir la cápsula por ese enlace, sin configurar
Wi-Fi. Para cargas largas ofrece una conexión Wi-Fi local guiada como modo rápido.
No necesita Internet durante esa transferencia ni retirar la SD.
El primer emparejamiento usa la prueba de posesión definida en el
[flujo de app](actualizacion-mapas.md).

La app distingue «Descargando al teléfono», «Enviando al equipo», «Verificando»
y «Listo para activar». Ante desconexión ofrece «Continuar»; al finalizar muestra
la cobertura y fecha efectivamente activas. El HUD autoriza la activación según
su política de mantenimiento. La app conserva el paquete hasta confirmar el éxito.
Ni nombres de archivos, ni tiles, ni offsets aparecen en este flujo.

La idea de producto es **recordar qué cobertura necesita el cliente**, para que
las siguientes actualizaciones sean un botón. Más adelante podría ofrecer
«Preparar mi viaje» a partir de un recorrido elegido, sin enviar historial de
conducción por defecto. Esa selección necesitaría cobertura de vecinos y desvíos
medida; no forma parte del primer instalador ni exige cambiar la geometría.

## Un contenedor que se consulta directamente

Cada edición de una cobertura se entrega como una cápsula inmutable, por ejemplo
`buenos-aires-2026-09.hudmap`. Es un contenedor indexado, no un archivo que el HUD
deba descomprimir para reconstruir `/tiles`. Nombre y extensión son ilustrativos;
el protocolo utiliza identidades de contenido y rutas internas.

```text
Cápsula = cabecera + manifiesto firmado + índice paginado + payloads v1
Consulta = origen de celda → offset/longitud → mismos bytes v1 → matcher
```

El índice relaciona el par de coordenadas enteras de origen usado por el lector
actual con un rango de bytes. Orden, escala y redondeo deben reproducir la búsqueda
actual, incluyendo coordenadas negativas. Cada payload conserva exactamente los
bytes del tile de origen. El empaquetador puede consumir los archivos del generador
existente; el equipo y el cliente dejan de depender de ese árbol de archivos.

El contenedor no arregla por sí mismo errores de cobertura, reglas de velocidad
ni heurísticas del matcher. Esas correcciones mantienen pruebas separadas.

## Contrato mínimo de almacenamiento

Extraer primero una fuente que entregue bytes acotados y estados tipados:

| Responsabilidad | Contrato propuesto |
| --- | --- |
| `map_source` | Buscar una celda en una generación; devolver bytes y longitud con ownership explícito, ausencia o error |
| `map_store` | Resolver fuente, caché, lectores activos, selección y cambio de generación |
| Acceso al contenedor | `read_at(offset, length)` y tamaño; sin rutas por celda |
| Instalador | Recibir una candidata inmutable, verificarla y solicitar activación |

La fuente de directorio conserva los datasets y replays actuales durante la
extracción. La fuente de cápsula implementa el mismo contrato. No mezclar fuentes
silenciosamente dentro de una consulta si una cápsula está dañada; el fallback
selecciona una generación completa y reinicia el estado de matching.

Primer soporte físico propuesto: un archivo por edición en la SD, conservando
activa y candidata/anterior. **Se elimina el filesystem de tiles; sigue existiendo
un filesystem para unos pocos contenedores.** El acceso por rangos permite estudiar
sectores reservados después, pero eliminar FAT exigiría provisión, asignación,
recuperación y pruebas propias. No es condición de ninguna integración.

## Índice y límites

La cabecera declara versión de contenedor, tamaños y posiciones. El manifiesto
liga identidad, cobertura, versión de contenido, parámetros geométricos,
compatibilidad del lector y hashes del índice/payloads. La versión del contenedor
es independiente de la versión geográfica y del firmware.

Propuesta inicial: entradas de ancho fijo ordenadas por coordenadas, páginas de
índice y un directorio superior también paginable. Autenticar cada página contra
una raíz ligada al manifiesto firmado; cada entrada liga el hash de su payload.
Una ausencia solo se confirma después de verificar el rango del índice que debía
contener la clave. No cargar todo el índice ni toda la cápsula en RAM.

Como cálculo orientativo, una entrada de 56 bytes —dos `int32`, offset `uint64`,
longitud `uint32`, flags `uint32` y SHA-256 de 32 bytes— ocuparía 5,6 MB para
100.000 celdas, más directorios y autenticación. Es sobrecarga de almacenamiento,
no una reserva RAM. Medirla junto a los payloads antes de congelar el formato.
La serialización, endianness, páginas, límites y esquema de firma se fijan con
vectores interoperables en T33; este documento no define todavía un formato wire.

Rechazar claves duplicadas, rangos superpuestos o fuera del archivo, overflow,
conteos excesivos, parámetros incompatibles y datos truncados. Acotar tamaño de
payload, profundidad de índice y memoria de validación. Para el primer corte,
payloads sin compresión: preservan bytes y lecturas por rango. Compresión por
bloques y actualizaciones diferenciales se evalúan solo después de medir.

## Actualización completa, reanudable y recuperable

1. La app consulta capacidades, edición activa y espacio necesario; obtiene una
   cápsula completa compatible. Una cobertura por operación en el primer corte.
2. El HUD reserva candidata y metadatos sin borrar activa ni fallback protegido.
   Recibe por bloques con ID de sesión ligado a la identidad de la cápsula.
3. Persiste checkpoints acotados. Tras corte, verifica qué prefijo sigue durable;
   devuelve el próximo offset recuperable. Reanudar otro paquete bajo el mismo
   ID se rechaza. Los tamaños de transporte e índice son independientes.
4. Verifica firma, índice, todos los payloads y su estructura con el parser real;
   abre la candidata mediante la misma fuente que usará el matcher.
5. En mantenimiento, aplica la barrera de lectores y selección recuperable
   descritas en [actualización de mapas](actualizacion-mapas.md). Solo entonces
   comunica la nueva versión activa. Conserva la edición anterior verificable.

Un checkpoint no se escribe en NVS por cada bloque recibido. Definir frecuencia
y journal acotados con el presupuesto de I/O. No presentar recepción en RAM como
progreso durable. Al arrancar, verificar selección/manifiesto y autenticar páginas
y payloads antes de usarlos; UART/UI arrancan independientemente de esas lecturas.
Conservar el límite como desconocido si no hay fuente confiable.

Primero se transfiere la cápsula completa, con reanudación por offset. Así no se
necesita un recolector de bloques compartidos, parches sobre el mapa activo ni un
catálogo de archivos por tile. Dos copias en una SD no protegen contra pérdida
física o corrupción de toda la tarjeta; se conserva recuperación desde app/servicio.

## Integración y evidencia pendiente

P04/T22 extrae la fuente con el backend actual. P07/T33–T39 añade cápsula e
instalación como incrementos separados. App y cliente de banco trabajan sobre
capacidades y operaciones de transferencia; UART, UI, Wi-Fi, Starlink y OTA no
dependen de terminar la cápsula ni de P06/R01. El cierre de instalación de cápsulas
sí requiere validar su backend; no se declara soportado antes de hacerlo.

Antes de entregar a clientes:

- Comparar los bytes de cada celda y resultados de replay entre directorio y
  cápsula, con caché fría y caliente y el mismo dataset.
- Medir tamaño total, RAM máxima, lecturas, latencia p95/p99 y tiempo de instalación
  en Waveshare con carga UART/UI; no dar por supuesto que agrupar mejora todo.
- Probar índice alterado, payload corrupto, celda ausente, coordenadas negativas,
  corte durante recepción/activación, SD llena/cambiada y firmware de rollback.
- Verificar el recorrido desde teléfono con descarga previa, cambio a Wi-Fi del
  HUD opcional, desconexión y continuación sin elegir archivos ni volver a emparejar.
  Completar también una cápsula representativa enteramente por BLE, medir duración
  e I/O y probar suspensión de app y rechazo del cambio de red. Ambos transportes
  usan el mismo instalador y formato; no requieren diferenciales ni v2.

v2/zonas queda para una evaluación posterior sobre esta base. Adoptarlo o
descartarlo no cambia el gesto del cliente ni el contrato del instalador.
