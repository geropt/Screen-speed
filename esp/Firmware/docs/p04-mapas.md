# P04 — Matcher puro, map_store y replay de equivalencia

Revisión: 2026-09-05. Extracción del matching sin cambiar resultados, con la prueba
de equivalencia que el plan exige como puerta.

## 1. La regla de esta etapa

El plan es explícito: «no cerrar una extracción aceptando diferencias bajo el
argumento de que ahora parece mejor». Así que la extracción se hizo contra un oráculo
congelado: el golden generado en **P00**, con el código viejo y su estado `static` de
función, antes de tocar nada.

Resultado: **13 de 13 filas idénticas**. La equivalencia se verifica en cada corrida
de `make -C esp/Firmware/host_tests test`, y el objetivo `update-golden` se eliminó
del Makefile a propósito — una diferencia significa que la extracción cambió el
comportamiento, no que haya que regenerar el archivo.

## 2. La división

`components/tile_reader` se partió en dos, con `git mv` para que el diff muestre
exactamente qué cambió:

| Componente | Responsabilidad | No sabe de |
| --- | --- | --- |
| `map_match` | Geometría, puntuación, stickiness, elección de vía | Archivos, rutas, tarjeta, caché |
| `map_store` | Caché, generación, ownership, rutas en la tarjeta | Geometría |

**La matemática no se tocó.** Umbrales, pesos de rumbo, bonus de stickiness, orden de
recorrido de vecinos, criterios de corte y el manejo de vías anónimas se movieron
byte por byte.

## 3. Lo que sí cambió

### Fuente de candidatos inyectable

Antes el matcher llamaba directo a `tile_cache_get()`, que abre archivos en la
tarjeta. Ahora recibe un `map_tile_source_t` con un puntero a función. Consecuencia
concreta: el replay de equivalencia usa una fuente que lee el directorio del dataset
**sin caché**, así que la equivalencia queda demostrada con otro backend de
almacenamiento. Mismo C, otra fuente, mismo resultado.

### Estado explícito con reset

Antes vivía en `static` de función dentro de `get_speed_and_name_at()`:

```c
static char locked_name[MAX_STREET_NAME];
static int  locked_speed = 0;
static bool have_lock = false;
static bool locked_anon = false;
```

Sin ninguna forma de limpiarlo. El resultado de un fix dependía de todos los
anteriores y dos replays en el mismo proceso se contaminaban. Ahora el estado vive en
`map_match_state_t` y `map_match_reset()` lo limpia.

Hay una prueba que verifica que el reset **no es decorativo**: después de una
consulta, `have_lock` es true y `locked_name` tiene contenido; después del reset,
ambos vuelven a cero. Y dos replays con reset dan exactamente lo mismo.

### Resultado tipado

`map_result_t` informa lo que antes se perdía: el origen del límite (vía con nombre o
anónima), las distancias a cada una, los tiles consultados, los ausentes, los
truncados, y la **generación** del dataset con que se calculó.

### Generación del dataset

`map_store` incrementa la generación en cada montaje. El firmware compara la
generación del resultado con la actual y lo descarta si el mapa cambió mientras se
leían tiles, en lugar de mostrar el límite de una calle de otra tarjeta. Es el mismo
mecanismo que `tracker_epoch` en el estado del vehículo, aplicado al almacenamiento.

### Sin mapas no se abre nada

`map_store_fetch()` devuelve NULL de entrada cuando no hay tarjeta. Antes el matcher
hacía las nueve aperturas igual y fallaban una por una.

## 4. Corrección posterior, en commit separado

El plan pide que las correcciones vayan **después** de demostrar equivalencia y en
cambios separados. La primera:

**Tiles truncados informados en lugar de aceptados en silencio.** El matcher siempre
toleró el truncamiento —deja de leer y conserva lo ya puntuado, que es correcto para
no perder un tramo válido por un archivo dañado al final— pero no lo decía. Y un
archivo más corto que su propio encabezado de 4 bytes se confundía con un tile vacío:
`segCount` quedaba en 0, el bucle no corría, y el resultado decía «acá no hay calles»
cuando lo correcto era «este archivo está dañado».

Ese hueco lo encontró **una aserción del test nuevo**, no una lectura del código.

La equivalencia sigue pasando después de la corrección, que es lo que la hace
confiable: el cambio agrega información sin mover ningún resultado.

## 5. Pruebas

| Suite | Cubre |
| --- | --- |
| `test_map_match_equivalence` | 13 fixes contra el golden de P00; reset determinista; generación en el resultado; origen del límite; null-island sin tocar la fuente |
| `test_map_match_robustness` | 10 casos con fuente sintética en memoria: truncamiento en cada offset bajo ASan, encabezado que miente la cantidad de segmentos, segmento que miente la cantidad de puntos, tile vacío, vía anónima, vía sin límite, barrido de 9 tiles contra 1 |

Los tiles sintéticos permiten llegar a casos que el dataset real no contiene, que es
lo que pide la fila «Mapas» de la matriz mínima de fallas.

## 6. Verificación ejecutada

| Qué | Resultado |
| --- | --- |
| Equivalencia v1 | 13/13 filas idénticas, 36 lecturas, 27 tiles ausentes |
| Build del HUD | Compila. 1 676 912 → 1 677 744 B (+832 B), IRAM sin cambio |
| Las 6 suites de host | En verde |
| Generador de tiles | `extract_tiles.py` apuntaba a la ruta vieja de `tile_config.h`; corregido |

## 7. Lo que sigue abierto

- **Nada se flasheó.** El comportamiento del matcher en la placa, con la caché de
  PSRAM y la tarjeta real, sigue sin medir. El p95 <200 ms de matching es una meta,
  no un dato.
- **Corpus de replay más grande.** El plan habla de «corpus acordado» y los seis logs
  de referencia que menciona el código; acá la equivalencia se probó sobre 13 fixes
  derivados del dataset. Es un oráculo válido pero chico: con los logs reales la
  prueba sería mucho más fuerte.
- **Correcciones pendientes**, cada una en su commit: caché negativa por ausencia
  contra error de E/S —hoy `tile_cache` no distingue «no existe» de «falló la
  lectura»—, política de expulsión bajo OOM, y barrera de lectores para el cambio de
  generación con consultas en curso. Esto último se cruza con P07.
- **`map_store` todavía no reporta errores de E/S**: el campo `io_errors` existe y
  siempre vale 0, porque `tile_cache_get()` devuelve NULL sin decir por qué.
