# P07 — Cápsula de mapas e instalador transaccional

Revisión: 2026-09-05. **No cierra P07**: la puerta del plan exige cortes físicos de
alimentación en la tarjeta elegida, y eso necesita la placa. Lo que hay acá es el
formato, el lector, el empaquetador, el instalador y la inyección de fallas en cada
transición, todo probado en host.

## 1. Equivalencia primero

El requisito textual del plan es «empaquetar los payloads actuales **sin cambiar su
binario**». La cápsula es un contenedor, no un formato nuevo de tile.

La prueba: el matcher alimentado desde la cápsula corre la misma secuencia de 13 fixes
y se compara contra el **mismo golden congelado en P00**, generado leyendo el
directorio antes de toda esta reestructuración. Resultado: **13 de 13 filas
idénticas**.

Comparar contra el golden y no contra el directorio es deliberado: ata el contenedor a
la referencia original, no a otra implementación que también podría haberse desviado.

## 2. El formato

```
[cabecera 64 B][manifiesto 156 B][índice ordenado][región de payloads v1]
```

El acceso es `read_at(offset, len)`: no hay rutas por celda ni nombres de archivo, y
buscar una celda es una búsqueda binaria sobre el índice.

| Decisión | Motivo |
| --- | --- |
| Entrada de índice de 24 B, no los 56 B que estimaba la propuesta | No se guarda hash completo por payload —CRC-32 alcanza para integridad— ni los campos que la estimación reservaba |
| El índice se lee por páginas de 512 B | Son 47 KiB con la cobertura actual; no hace falta cargarlo entero |
| El manifiesto declara la geometría | Una cápsula con otra geometría haría buscar celdas con el tamaño equivocado, y el resultado sería silenciosa y sistemáticamente incorrecto. Hay una prueba de que coincide con la que compila el matcher |
| Un lector más viejo rechaza la cápsula | Interpretar un formato desconocido daría límites de velocidad equivocados |
| Un payload con CRC malo no se entrega | Darle bytes dañados al matcher produciría un límite inventado |

### Medición del tamaño

| Qué | Tamaño |
| --- | --- |
| Payloads (suma de los 1 961 tiles) | 3 028 397 B |
| Índice | 47 064 B |
| Cápsula total | 3 075 681 B |
| El mismo directorio en disco | 8,0 MiB |

Los archivos chicos gastan unos 5 MiB en slack de bloque, que el contenedor elimina.
Es un dato del sistema de archivos del host: en la FAT de la tarjeta el tamaño de
cluster cambia la cifra, pero el sentido es el mismo.

### Integridad, no autenticidad

Los CRC-32 detectan corrupción —escritura interrumpida, tarjeta dañada, transferencia
truncada— y **no** detectan manipulación: cualquiera que edite el contenido puede
recalcularlos. El campo de firma del manifiesto existe, está en cero, y hay una prueba
que lo asevera para que nadie lo lea como verificado. Firmar y verificar contra una
clave es trabajo de P10.

Esto queda escrito en el encabezado del componente, no sólo acá.

## 3. El instalador

```
IDLE --begin--> RECEIVING --finish--> VERIFYING --ok--> READY --activate--> IDLE
  ^                 |                     |              |
  +---- cancel -----+------ inválida -----+--------------+
```

| Garantía | Cómo se sostiene |
| --- | --- |
| Una candidata inválida nunca se activa | `activate()` sólo existe desde READY. No hay camino desde RECEIVING ni desde FAILED: es estructural, no una comprobación que se pueda olvidar |
| La activa sobrevive a cualquier corte | La candidata nunca es la activa; se recibe siempre en la otra edición, elegida internamente |
| Si el selector no se puede escribir, la activa anterior sigue activa | No queda un estado intermedio sin selección, y la candidata verificada se conserva para reintentar |
| El progreso informado es durable | Cada chunk se sincroniza antes de contar; ese offset es desde donde el emisor reanuda, e informar un avance que un corte se lleva puesto perdería datos en silencio |
| Chunks idempotentes | Un reenvío idéntico se acepta sin reescribir, porque una reanudación tras corte de enlace lo produce naturalmente |
| El mismo offset con contenido distinto es un error | Significa que el emisor cambió de paquete a mitad de camino: no es reanudación |
| Sin huecos | Un offset salteado dejaría una zona sin escribir que podría contener lo que había antes en la tarjeta |
| Paths internos | El cliente nunca pasa una ruta; los nombres los decide el instalador |

### Reconciliación de arranque

No se puede asumir atomicidad entre el selector (NVS) y el contenido (SD): un corte
entre escribir el selector y terminar de sincronizar la tarjeta deja las dos cosas en
desacuerdo. `map_installer_recover()` verifica lo que el selector dice que está activo;
si no verifica, vuelve a la otra edición si esa sí verifica; y si ninguna verifica queda
**sin mapas de forma explícita**.

Sin mapas es mejor que un mapa a medias: un mapa parcial daría límites de velocidad de
zonas equivocadas. Y por P02b/P03, sin mapas el velocímetro sigue funcionando y la
pantalla dice «Sin mapas» en lugar de mostrar un número inventado.

## 4. Inyección de fallas

El almacenamiento se inyecta, así que se puede fallar en **cada** transición
persistente y comprobar qué edición quedó activa:

| Falla en | Resultado verificado |
| --- | --- |
| `reserve` | No arranca la sesión; activa intacta |
| `write` | Error de E/S; no se puede activar; activa intacta |
| `write` a mitad de la transferencia | `finish` rechaza incompleta; activa intacta; reanudación desde el offset durable funciona |
| `sync` | El avance NO se informa como durable |
| `verify` | Estado FAILED; `activate` rechazado; activa intacta |
| `commit_selector` | Activa anterior sigue activa; candidata conservada; reintento funciona |

Sin inyectar la falla en cada punto, «los cortes no activan una candidata inválida»
sería una intención y no un hecho.

## 5. Verificación ejecutada

| Qué | Resultado |
| --- | --- |
| `test_capsule_equivalence` | 13/13 filas idénticas al golden de P00, 9 180 lecturas por rangos |
| `test_map_installer` | 20 casos con fallas en todas las transiciones |
| Empaquetador sobre el dataset real | 1 961 celdas, 3 075 681 B |
| Build del HUD | Compila. 1 677 744 → 1 678 272 B (+528 B) |
| Las 11 suites de host | En verde |

## 6. Lo que falta para cerrar P07

1. **Cortes físicos de alimentación en la tarjeta elegida.** El plan los pide
   explícitamente. El almacenamiento simulado prueba la máquina de estados, **no** el
   comportamiento de la FAT ante un corte real: escrituras reordenadas, sectores a
   medio escribir y el caché de la controladora de la tarjeta no están modelados.
2. **Medir la lectura por rangos en la tarjeta.** 9 180 lecturas para 13 fixes en el
   host es barato; en la FAT hay que medirlo antes de dar el backend por bueno, y
   decidir si conviene una caché por encima de la cápsula.
3. **Barrera de lectores.** Cambiar de generación con consultas en curso todavía no
   tiene barrera: el plan pide «si no drena lectores, posponer sin forzar
   liberaciones», y eso no está implementado. Quedó anotado desde P04.
4. **Firma del manifiesto**: reservada, en cero. P10.
5. **Cliente de banco end-to-end**: el instalador tiene la máquina de estados pero
   nadie lo llama todavía. Falta el transporte —`begin/write/status/finish/activate/
   cancel` sobre BLE o Wi-Fi local— que es P08.
6. **Limpieza de contenido inactivo**: el plan pide limpiar «sólo contenido inactivo y
   elegible». Hoy `discard_candidate` borra la candidata, pero no hay política de
   retención de ediciones viejas.
7. **Espacio real**: `reserve` está inyectado, así que el cálculo de si una cápsula
   entra en la tarjeta —con la activa todavía presente— no está implementado.
