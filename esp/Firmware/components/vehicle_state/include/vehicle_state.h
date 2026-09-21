/* Estado del vehículo: una sola fuente de verdad, con edad por señal.
 *
 * Problema que resuelve. Antes cada dato del Ruptela viajaba solo y el consumidor
 * no tenía forma de saber cuán viejo era: un fix se copiaba entero a una cola de
 * 5 lugares —así que un consumidor lento acumulaba fixes vencidos y los procesaba
 * como si fueran actuales— y las señales de IO no existían en el HUD.
 *
 * Reglas de diseño:
 *
 *  - **Cada señal tiene su propia edad.** Que llegue un fix no rejuvenece el
 *    estado de GPRS, y que llegue un record cualquiera no rejuvenece la ignición.
 *    El plan lo pide explícitamente: «no renovar 418 porque llegó otro record».
 *  - **Tres estados por señal**: desconocida (nunca llegó), fresca (dentro de su
 *    TTL) y vencida (llegó, pero hace demasiado). «Vencida» y «desconocida» son
 *    distintas y la UI debe poder distinguirlas.
 *  - **`tracker_epoch`**: cambia cuando cambia el IMEI. Todo lo acumulado
 *    pertenece a la época en que se recibió, así que al cambiar de tracker el
 *    estado anterior se invalida en lugar de mezclarse. Un resultado calculado
 *    con una época vieja se puede rechazar comparando el número.
 *  - **Snapshot coherente**: el consumidor obtiene todo de una sola vez, con las
 *    edades calculadas al mismo instante. No hay forma de leer una posición de un
 *    momento y una velocidad de otro.
 *  - **El reloj entra por parámetro.** Ninguna función lee el reloj por su
 *    cuenta: `now_ms` es siempre argumento. Eso hace al componente puro,
 *    determinista y probable en el host, y obliga al llamador a usar un reloj
 *    monotónico (en el dispositivo, `esp_timer_get_time()`), no la hora del GPS,
 *    que salta.
 *
 * Sin dependencias de ESP-IDF. Sin memoria dinámica. Sin locks: el dueño del
 * struct es una sola tarea, y los productores le mandan mensajes acotados.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VEHICLE_IMEI_MAX_LEN 16

/* Lecturas consecutivas coincidentes que hacen falta para aceptar un cambio de IMEI.
 *
 * El marcador `###IMEI` viaja sin checksum. Medido sobre una captura de campo: en
 * 1 809 lecturas apareció una corrupta, con un dígito de más. Aceptar una sola lectura
 * discrepante habría subido `tracker_epoch` y descartado posición, ignición y GPRS por
 * un byte perdido en la serie. El marcador se repite varias veces por segundo, así que
 * exigir dos coincidencias no retrasa un cambio real y descarta el ruido. */
#define VEHICLE_IMEI_CONFIRMATIONS 2

/** Estado de frescura de una señal. */
typedef enum {
    VS_UNKNOWN = 0,  /**< nunca se recibió                    */
    VS_FRESH,        /**< recibida y dentro de su TTL          */
    VS_EXPIRED       /**< recibida, pero venció                */
} vs_state_t;

/** TTL por señal. Cada una tiene el suyo porque cambian a ritmos distintos. */
typedef struct {
    uint32_t fix_ttl_ms;       /**< posición y velocidad          */
    uint32_t ignition_ttl_ms;  /**< IO 409                        */
    uint32_t gprs_ttl_ms;      /**< IO 418                        */
} vehicle_state_config_t;

/* El fix se declara vencido a los 3 s: el plan fija «probar 3 s iniciales a 1 Hz»
 * y a esa cadencia tres segundos son tres fixes perdidos, suficiente para dejar
 * de presentar la posición como actual.
 *
 * Las señales de IO llegan mucho más espaciadas —dependen de la configuración del
 * tracker— así que 30 s evita declararlas vencidas por su cadencia normal. Son
 * valores iniciales para negociar con mediciones reales, no requisitos medidos. */
#define VEHICLE_STATE_CONFIG_DEFAULT() { \
    .fix_ttl_ms = 3000,                  \
    .ignition_ttl_ms = 30000,            \
    .gprs_ttl_ms = 30000,                \
}

/** Mensaje acotado que los productores le mandan al dueño del estado. */
typedef enum {
    VEHICLE_MSG_FIX = 0,
    VEHICLE_MSG_IGNITION,
    VEHICLE_MSG_GPRS,
    VEHICLE_MSG_IMEI,
} vehicle_msg_kind_t;

typedef struct {
    vehicle_msg_kind_t kind;
    uint64_t mono_ms;               /**< reloj monotónico al recibir el dato */
    union {
        struct {
            double lat;
            double lon;
            float  speed_kmh;
            float  cog_deg;
        } fix;
        struct { bool on; }  ignition;
        struct { bool up; }  gprs;
        struct { char digits[VEHICLE_IMEI_MAX_LEN + 1]; } imei;
    } u;
} vehicle_msg_t;

/** Estado interno. El consumidor no lo lee directamente: pide un snapshot. */
typedef struct {
    vehicle_state_config_t cfg;

    uint64_t tracker_epoch;      /**< sube al cambiar el IMEI            */

    bool     fix_seen;
    uint64_t fix_mono_ms;
    double   fix_lat;
    double   fix_lon;
    float    fix_speed_kmh;
    float    fix_cog_deg;
    uint64_t fix_epoch;          /**< época en que se recibió el fix      */

    bool     ignition_seen;
    uint64_t ignition_mono_ms;
    bool     ignition_on;

    bool     gprs_seen;
    uint64_t gprs_mono_ms;
    bool     gprs_up;

    bool     imei_known;
    char     imei[VEHICLE_IMEI_MAX_LEN + 1];
    /* IMEI discrepante a la espera de confirmación. */
    char     imei_pending[VEHICLE_IMEI_MAX_LEN + 1];
    uint8_t  imei_pending_count;

    /* Contadores para diagnóstico. */
    uint32_t fixes;
    uint32_t ignition_updates;
    uint32_t gprs_updates;
    uint32_t imei_changes;
    /** Lecturas de IMEI discrepantes descartadas por falta de confirmación. */
    uint32_t imei_rejected_single;
    uint32_t rejected_stale_clock;
} vehicle_state_t;

/** Vista coherente para el consumidor, con todas las edades al mismo instante. */
typedef struct {
    uint64_t   mono_ms;          /**< instante del snapshot               */
    uint64_t   tracker_epoch;

    vs_state_t fix_state;
    uint32_t   fix_age_ms;
    double     lat;
    double     lon;
    float      speed_kmh;
    float      cog_deg;
    bool       fix_epoch_current;/**< el fix pertenece a la época actual  */

    vs_state_t ignition_state;
    uint32_t   ignition_age_ms;
    bool       ignition_on;

    vs_state_t gprs_state;
    uint32_t   gprs_age_ms;
    bool       gprs_up;

    bool       imei_known;
    char       imei[VEHICLE_IMEI_MAX_LEN + 1];
} vehicle_snapshot_t;

/**
 * @brief Inicializa el estado.
 *
 * @param cfg NULL usa VEHICLE_STATE_CONFIG_DEFAULT
 */
void vehicle_state_init(vehicle_state_t *vs, const vehicle_state_config_t *cfg);

/**
 * @brief Aplica un mensaje.
 *
 * Un mensaje con `mono_ms` anterior al último aplicado de la misma señal se
 * descarta y se cuenta en `rejected_stale_clock`: el reloj es monotónico, así que
 * eso indica un mensaje reordenado, no un dato nuevo.
 *
 * @return true si el mensaje modificó el estado
 */
bool vehicle_state_apply(vehicle_state_t *vs, const vehicle_msg_t *msg);

/** Construye el snapshot con las edades calculadas a `now_ms`. */
void vehicle_state_snapshot(const vehicle_state_t *vs, uint64_t now_ms,
                           vehicle_snapshot_t *out);

/**
 * @brief Invalida todo lo recibido y arranca una época nueva.
 *
 * Lo usa internamente el cambio de IMEI. Se expone porque un remount de la
 * tarjeta o un reinicio del enlace también pueden necesitarlo.
 */
void vehicle_state_new_epoch(vehicle_state_t *vs);

/** Texto corto para logs. */
const char *vs_state_name(vs_state_t s);

#ifdef __cplusplus
}
#endif
