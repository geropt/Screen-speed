# P03 — UI mantenible y velocidad independiente

Revisión: 2026-09-05. Cierra la parte de P03 que se puede hacer sin la unidad.
**No aprueba la etapa**: el plan exige «screenshots/fotos de estados de falla» y
«velocidad fluida con SD lenta», y nada de eso sale sin flashear.

## 1. El hallazgo que definió el diseño

`tick_screen_main()` —generado por EEZ— **reescribe `current_speed_label` y
`speed_limit_label` en cada tick** a partir de las variables de flow:

```c
const char *new_val = evalTextProperty(flowState, 2, 3, ...);
const char *cur_val = lv_label_get_text(objects.current_speed_label);
if (strcmp(new_val, cur_val) != 0) {
    lv_label_set_text(objects.current_speed_label, new_val);
}
```

Escribir esos labels desde un presenter propio habría dejado dos escritores sobre
el mismo widget: exactamente el problema que la etapa viene a eliminar. El reparto
final respeta la propiedad del código generado en lugar de pelearla:

| Dueño | Widgets |
| --- | --- |
| EEZ (generado) | `current_speed_label`, `speed_limit_label` — el presenter les habla por `set_var_*()` |
| Presenter | `speed_limit_warning_label`, `street_name`, `speed_limit_container`, `overspeed_ring` |

Ninguno de los cuatro del presenter aparece en `tick_screen_main()`, y **no se
modificó ningún archivo generado**, así que regenerar desde EEZ no borra esto. Eso
responde al requisito del plan de comprobar que regenerar no elimina lógica manual:
la lógica manual ya no vive en archivos generables.

## 2. Único escritor de widgets

Antes había tres escritores:

| Escritor | Dónde | Problema |
| --- | --- | --- |
| Tarea principal | `set_street_name()` | Llamaba a `lv_label_set_text` **sin el mutex** (corregido en P02b) |
| `calculation_task` | `calculate_threshold()` → anillo | Otra tarea tocando widgets cada 100 ms |
| Tarea de LVGL | `tick_screen_main()` | El legítimo |

Ahora todas las escrituras ocurren en `ui_presenter_tick()`, dentro de la tarea de
LVGL con el mutex ya tomado, llamado **antes** de `ui_tick()` porque publica las
variables que el código generado pinta enseguida.

La única excepción admitida es la señal de flush-ready del callback de DMA, que
cierra la traza y no toca widgets. Es la misma excepción que el plan permite.

El modelo viaja por un buzón con copia y espera acotada de 20 ms: la tarea
principal nunca espera a LVGL ni al panel. Si el modelo no cambió, no se repinta.

## 3. Estados que no mienten

### Límite

| Estado | En pantalla | Cuándo |
| --- | --- | --- |
| Desconocido | contenedor oculto, «sin dato» | Nunca se resolvió un límite |
| Conocido | número solo | Resuelto en este fix |
| Último conocido | número + «ultimo» | Resuelto antes; la posición ya no lo confirma |
| Inferido | número + «inferido» | Vía sin nombre: no se puede atribuir a una calle |
| Vencido | contenedor oculto, «vencido» | Pasó el TTL sin confirmación |

El último límite se **conserva** —el cliente lo pidió— pero su marca de tiempo no
se refresca cuando el matcher deja de confirmarlo, así que envejece hasta declararse
vencido en lugar de seguir presentándose como si fuera del tramo actual.

Vencido y desconocido esconden el número. Un entero no puede expresar «no sé»:
poner 0 se leería como «límite 0 km/h».

### Velocidad

Desconocida, en vivo, o vencida (número marcado como no actual). **Se muestra en
cuanto hay un fix válido, sin esperar el matching y sin depender de la tarjeta**,
que es el requisito central de la etapa.

### Límite inicial ficticio, retirado

`vars.c` tenía `speed_limit = 78`. La pantalla mostraba «78 km/h» desde el arranque,
antes de leer un solo tile y sin saber en qué calle está el vehículo. Ahora arranca
en 0 y el presenter esconde el contenedor mientras el estado no sea defendible.

## 4. Tolerancia y alerta como lógica pura

`calculate_threshold()` leía las variables de la UI, comparaba y prendía el titileo
del anillo, todo junto con la animación. No se podía probar sin una pantalla.

Ahora la decisión vive en `components/ui_model`, con **histéresis de dos umbrales**:
enciende al superar `límite + 5` y apaga al bajar de `límite + 2`. Dos umbrales
distintos son lo que evita el titileo cuando la velocidad oscila alrededor del
límite.

Reglas de habilitación, todas probadas:

- No se alerta con el fix vencido.
- No se alerta con el límite vencido o desconocido.
- No se alerta por debajo de 5 km/h, donde el rumbo y la velocidad del GPS son poco
  confiables.
- Sí se alerta con un límite declarado como inferencia: es un dato, aunque no se
  pueda atribuir a una calle.

`dynamic.c` quedó sin lógica y `calculation_task` se eliminó: existía sólo para
llamar a `calculate_threshold()` cada 100 ms.

## 5. Traza recepción → snapshot → flush

La tarea principal marca el instante del fix con `ui_presenter_note_fix_received()`
y el callback de DMA del panel la cierra con `ui_presenter_note_flush_done()`,
registrando la muestra en `RES_CH_FIX_TO_DISPLAY`.

Eso mide de punta a punta lo que el plan quiere acotar: **p95 <200 ms desde el fix
hasta el flush del dato**. `s_trace_armed` evita atribuirle a un fix el flush de una
animación que no tiene nada que ver. La cifra sólo existirá cuando se flashee.

## 6. Verificación ejecutada

| Qué | Resultado |
| --- | --- |
| `test_ui_model` | 23 casos con ASan/UBSan, en verde |
| Build del HUD | Compila. 1 675 344 → 1 676 912 B (+1 568 B), DIRAM +544 B (buzón de dos modelos), IRAM sin cambio |
| Las 6 suites de host | En verde; el golden del matcher no se movió |

## 7. Lo que falta para cerrar P03

1. **Fotos de la pantalla en cada estado**: desconocido, último conocido, inferido,
   vencido, sin fix, sin mapas, exceso activo. El plan las pide explícitamente y no
   hay forma de obtenerlas sin la placa.
2. **Velocidad fluida con SD lenta**: hay que comprobar que el matching no atrasa la
   actualización de velocidad. La instrumentación ya está; falta la medición.
3. **Traza completa medida**: `RES_CH_FIX_TO_DISPLAY` contra la meta de 200 ms.
4. **Revisar la posición del cualificador** en pantalla. Se reutiliza
   `speed_limit_warning_label`, que ya existía en el proyecto EEZ; puede necesitar
   ajuste de layout que sólo se ve en la pantalla real.
5. **Modo servicio y pantallas de mantenimiento**: el shell mínimo que pide el plan
   no se hizo. El estado ya distingue ignición desconocida de apagada, pero no hay
   pantalla de servicio, y agregarla sin función detrás sería simular algo que no
   existe.
6. **Navegación**: hay una sola pantalla (`SCREEN_ID_MAIN`). No hay navegación que
   verificar todavía, así que «sin referencias colgantes» es trivialmente cierto y
   no cuenta como evidencia.
