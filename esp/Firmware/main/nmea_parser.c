/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nmea_parser.h"
#include "nmea_framer.h"
#include "ruptela_io_parser.h"
#include "imei_scanner.h"
#include "res_metrics.h"

/**
 * @brief NMEA Parser runtime buffer size
 *
 */
#define NMEA_PARSER_RUNTIME_BUFFER_SIZE (CONFIG_NMEA_PARSER_RING_BUFFER_SIZE / 2)
#define NMEA_MAX_STATEMENT_ITEM_LENGTH (16)
#define NMEA_EVENT_LOOP_QUEUE_SIZE (16)

/* Pro5-Lite / HCV5-Lite tracker units cap the transparent channel at 9600 baud
 * (Ruptela "Transparent Channel Configuration" doc); Pro5 / HCV5 units default
 * to 115200. Probe both so the same firmware binary works with either. */
static const uint32_t nmea_baud_candidates[] = { 9600, 115200 };
#define NMEA_BAUD_PROBE_MS (3000)
#define NMEA_BAUD_RELOCK_MS (30000)

/**
 * @brief Define of NMEA Parser Event base
 *
 */
ESP_EVENT_DEFINE_BASE(ESP_NMEA_EVENT);

static const char *GPS_TAG = "nmea_parser";

/**
 * @brief GPS parser library runtime structure
 *
 */
typedef struct {
    uint8_t item_pos;                              /*!< Current position in item */
    uint8_t item_num;                              /*!< Current item number */
    uint8_t asterisk;                              /*!< Asterisk detected flag */
    uint8_t crc;                                   /*!< Calculated CRC value */
    uint8_t parsed_statement;                      /*!< OR'd of statements that have been parsed */
    uint8_t sat_num;                               /*!< Satellite number */
    uint8_t sat_count;                             /*!< Satellite count */
    uint8_t cur_statement;                         /*!< Current statement ID */
    uint32_t all_statements;                       /*!< All statements mask */
    char item_str[NMEA_MAX_STATEMENT_ITEM_LENGTH]; /*!< Current item */
    gps_t parent;                                  /*!< Parent class */
    uart_port_t uart_port;                         /*!< Uart port number */
    uint8_t *buffer;                               /*!< Runtime buffer */
    esp_event_loop_handle_t event_loop_hdl;        /*!< Event loop handle */
    TaskHandle_t tsk_hdl;                          /*!< NMEA Parser task handle */
    QueueHandle_t event_queue;                     /*!< UART event queue handle */
    uint32_t baud_rate;                            /*!< Baud rate currently in use */
    uint8_t baud_idx;                              /*!< Index into nmea_baud_candidates */
    bool baud_locked;                              /*!< True once a valid checksum has been seen */
    TickType_t last_valid_tick;                    /*!< Tick of the last statement with a valid checksum */
    /* P02a: el framing dejó de depender de '\n' / NUL como frontera de
     * transporte. Los bytes crudos entran acá y salen sentencias completas con
     * checksum ya validado. Implementación compartida con el piloto en
     * esp/components/ruptela_framing. */
    nmea_framer_t framer;                          /*!< Framer de sentencias, binario-seguro */
    /* P02b: el mismo flujo de bytes alimenta además el scanner de records de IO y
     * el del marcador `###IMEI`. Los tres consumidores ven los mismos bytes
     * crudos; ninguno depende de que otro haya encontrado su frontera. */
    ruptela_io_parser_t io_parser;                 /*!< Records binarios de IO (409/418) */
    imei_scanner_t imei_scanner;                   /*!< Marcador `###IMEI` con arrastre */
    nmea_ignition_cb_t ignition_cb;
    nmea_gprs_cb_t gprs_cb;
    nmea_imei_cb_t imei_cb;
    nmea_record_cb_t record_cb;
    void *signal_ctx;
    /* Buffer propio para entregar la sentencia al parseo de campos. NO se puede
     * reusar `buffer`: ahí están los bytes crudos que el framer todavía está
     * recorriendo cuando invoca el callback, y sobrescribirlos corrompería el
     * resto del bloque leído. */
    uint8_t sentence_buf[NMEA_FRAMER_MAX_SENTENCE + 2];
    uint32_t rejected_invalid_fix;                 /*!< RMC con checksum válido y status 'V' */
    uint32_t rejected_out_of_range;                /*!< RMC con lat/lon no finita o fuera de rango */
} esp_gps_t;

/**
 * @brief parse latitude or longitude
 *              format of latitude in NMEA is ddmm.sss and longitude is dddmm.sss
 * @param esp_gps esp_gps_t type object
 * @return float Latitude or Longitude value (unit: degree)
 */
static float parse_lat_long(esp_gps_t *esp_gps)
{
    float ll = strtof(esp_gps->item_str, NULL);
    int deg = ((int)ll) / 100;
    float min = ll - (deg * 100);
    ll = deg + min / 60.0f;
    return ll;
}

/**
 * @brief Converter two continuous numeric character into a uint8_t number
 *
 * @param digit_char numeric character
 * @return uint8_t result of converting
 */
static inline uint8_t convert_two_digit2number(const char *digit_char)
{
    return 10 * (digit_char[0] - '0') + (digit_char[1] - '0');
}

/**
 * @brief Parse UTC time in GPS statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_utc_time(esp_gps_t *esp_gps)
{
    esp_gps->parent.tim.hour = convert_two_digit2number(esp_gps->item_str + 0);
    esp_gps->parent.tim.minute = convert_two_digit2number(esp_gps->item_str + 2);
    esp_gps->parent.tim.second = convert_two_digit2number(esp_gps->item_str + 4);
    if (esp_gps->item_str[6] == '.') {
        uint16_t tmp = 0;
        uint8_t i = 7;
        while (esp_gps->item_str[i]) {
            tmp = 10 * tmp + esp_gps->item_str[i] - '0';
            i++;
        }
        esp_gps->parent.tim.thousand = tmp;
    }
}

#if CONFIG_NMEA_STATEMENT_GGA
/**
 * @brief Parse GGA statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gga(esp_gps_t *esp_gps)
{
    /* Process GGA statement */
    switch (esp_gps->item_num) {
    case 1: /* Process UTC time */
        parse_utc_time(esp_gps);
        break;
    case 2: /* Latitude */
        esp_gps->parent.latitude = parse_lat_long(esp_gps);
        break;
    case 3: /* Latitude north(1)/south(-1) information */
        if (esp_gps->item_str[0] == 'S' || esp_gps->item_str[0] == 's') {
            esp_gps->parent.latitude *= -1;
        }
        break;
    case 4: /* Longitude */
        esp_gps->parent.longitude = parse_lat_long(esp_gps);
        break;
    case 5: /* Longitude east(1)/west(-1) information */
        if (esp_gps->item_str[0] == 'W' || esp_gps->item_str[0] == 'w') {
            esp_gps->parent.longitude *= -1;
        }
        break;
    case 6: /* Fix status */
        esp_gps->parent.fix = (gps_fix_t)strtol(esp_gps->item_str, NULL, 10);
        break;
    case 7: /* Satellites in use */
        esp_gps->parent.sats_in_use = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
        break;
    case 8: /* HDOP */
        esp_gps->parent.dop_h = strtof(esp_gps->item_str, NULL);
        break;
    case 9: /* Altitude */
        esp_gps->parent.altitude = strtof(esp_gps->item_str, NULL);
        break;
    case 11: /* Altitude above ellipsoid */
        esp_gps->parent.altitude += strtof(esp_gps->item_str, NULL);
        break;
    default:
        break;
    }
}
#endif

#if CONFIG_NMEA_STATEMENT_GSA
/**
 * @brief Parse GSA statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gsa(esp_gps_t *esp_gps)
{
    /* Process GSA statement */
    switch (esp_gps->item_num) {
    case 2: /* Process fix mode */
        esp_gps->parent.fix_mode = (gps_fix_mode_t)strtol(esp_gps->item_str, NULL, 10);
        break;
    case 15: /* Process PDOP */
        esp_gps->parent.dop_p = strtof(esp_gps->item_str, NULL);
        break;
    case 16: /* Process HDOP */
        esp_gps->parent.dop_h = strtof(esp_gps->item_str, NULL);
        break;
    case 17: /* Process VDOP */
        esp_gps->parent.dop_v = strtof(esp_gps->item_str, NULL);
        break;
    default:
        /* Parse satellite IDs */
        if (esp_gps->item_num >= 3 && esp_gps->item_num <= 14) {
            esp_gps->parent.sats_id_in_use[esp_gps->item_num - 3] = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
        }
        break;
    }
}
#endif

#if CONFIG_NMEA_STATEMENT_GSV
/**
 * @brief Parse GSV statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gsv(esp_gps_t *esp_gps)
{
    /* Process GSV statement */
    switch (esp_gps->item_num) {
    case 1: /* total GSV numbers */
        esp_gps->sat_count = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
        break;
    case 2: /* Current GSV statement number */
        esp_gps->sat_num = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
        break;
    case 3: /* Process satellites in view */
        esp_gps->parent.sats_in_view = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
        break;
    default:
        if (esp_gps->item_num >= 4 && esp_gps->item_num <= 19) {
            uint8_t item_num = esp_gps->item_num - 4; /* Normalize item number from 4-19 to 0-15 */
            uint8_t index;
            uint32_t value;
            index = 4 * (esp_gps->sat_num - 1) + item_num / 4; /* Get array index */
            if (index < GPS_MAX_SATELLITES_IN_VIEW) {
                value = strtol(esp_gps->item_str, NULL, 10);
                switch (item_num % 4) {
                case 0:
                    esp_gps->parent.sats_desc_in_view[index].num = (uint8_t)value;
                    break;
                case 1:
                    esp_gps->parent.sats_desc_in_view[index].elevation = (uint8_t)value;
                    break;
                case 2:
                    esp_gps->parent.sats_desc_in_view[index].azimuth = (uint16_t)value;
                    break;
                case 3:
                    esp_gps->parent.sats_desc_in_view[index].snr = (uint8_t)value;
                    break;
                default:
                    break;
                }
            }
        }
        break;
    }
}
#endif

#if CONFIG_NMEA_STATEMENT_RMC
/**
 * @brief Parse RMC statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_rmc(esp_gps_t *esp_gps)
{
    /* Process GPRMC statement */
    switch (esp_gps->item_num) {
    case 1:/* Process UTC time */
        parse_utc_time(esp_gps);
        break;
    case 2: /* Process valid status */
        esp_gps->parent.valid = (esp_gps->item_str[0] == 'A');
        break;
    case 3:/* Latitude */
        esp_gps->parent.latitude = parse_lat_long(esp_gps);
        break;
    case 4: /* Latitude north(1)/south(-1) information */
        if (esp_gps->item_str[0] == 'S' || esp_gps->item_str[0] == 's') {
            esp_gps->parent.latitude *= -1;
        }
        break;
    case 5: /* Longitude */
        esp_gps->parent.longitude = parse_lat_long(esp_gps);
        break;
    case 6: /* Longitude east(1)/west(-1) information */
        if (esp_gps->item_str[0] == 'W' || esp_gps->item_str[0] == 'w') {
            esp_gps->parent.longitude *= -1;
        }
        break;
    case 7: /* Process ground speed in unit m/s */
        esp_gps->parent.speed = strtof(esp_gps->item_str, NULL) * 0.514444;
        break;
    case 8: /* Process true course over ground */
        esp_gps->parent.cog = strtof(esp_gps->item_str, NULL);
        break;
    case 9: /* Process date */
        esp_gps->parent.date.day = convert_two_digit2number(esp_gps->item_str + 0);
        esp_gps->parent.date.month = convert_two_digit2number(esp_gps->item_str + 2);
        esp_gps->parent.date.year = convert_two_digit2number(esp_gps->item_str + 4);
        break;
    case 10: /* Process magnetic variation */
        esp_gps->parent.variation = strtof(esp_gps->item_str, NULL);
        break;
    default:
        break;
    }
}
#endif

#if CONFIG_NMEA_STATEMENT_GLL
/**
 * @brief Parse GLL statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gll(esp_gps_t *esp_gps)
{
    /* Process GPGLL statement */
    switch (esp_gps->item_num) {
    case 1:/* Latitude */
        esp_gps->parent.latitude = parse_lat_long(esp_gps);
        break;
    case 2: /* Latitude north(1)/south(-1) information */
        if (esp_gps->item_str[0] == 'S' || esp_gps->item_str[0] == 's') {
            esp_gps->parent.latitude *= -1;
        }
        break;
    case 3: /* Longitude */
        esp_gps->parent.longitude = parse_lat_long(esp_gps);
        break;
    case 4: /* Longitude east(1)/west(-1) information */
        if (esp_gps->item_str[0] == 'W' || esp_gps->item_str[0] == 'w') {
            esp_gps->parent.longitude *= -1;
        }
        break;
    case 5:/* Process UTC time */
        parse_utc_time(esp_gps);
        break;
    case 6: /* Process valid status */
        esp_gps->parent.valid = (esp_gps->item_str[0] == 'A');
        break;
    default:
        break;
    }
}
#endif

#if CONFIG_NMEA_STATEMENT_VTG
/**
 * @brief Parse VTG statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_vtg(esp_gps_t *esp_gps)
{
    /* Process GPVGT statement */
    switch (esp_gps->item_num) {
    case 1: /* Process true course over ground */
        esp_gps->parent.cog = strtof(esp_gps->item_str, NULL);
        break;
    case 3:/* Process magnetic variation */
        esp_gps->parent.variation = strtof(esp_gps->item_str, NULL);
        break;
    case 5:/* Process ground speed in unit m/s */
        esp_gps->parent.speed = strtof(esp_gps->item_str, NULL) * 0.514444;//knots to m/s
        break;
    case 7:/* Process ground speed in unit m/s */
        esp_gps->parent.speed = strtof(esp_gps->item_str, NULL) / 3.6;//km/h to m/s
        break;
    default:
        break;
    }
}
#endif

/**
 * @brief Parse received item
 *
 * @param esp_gps esp_gps_t type object
 * @return esp_err_t ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t parse_item(esp_gps_t *esp_gps)
{
    esp_err_t err = ESP_OK;
    /* start of a statement */
    if (esp_gps->item_num == 0 && esp_gps->item_str[0] == '$') {
        if (0) {
        }
#if CONFIG_NMEA_STATEMENT_GGA
        else if (strstr(esp_gps->item_str, "GGA")) {
            esp_gps->cur_statement = STATEMENT_GGA;
        }
#endif
#if CONFIG_NMEA_STATEMENT_GSA
        else if (strstr(esp_gps->item_str, "GSA")) {
            esp_gps->cur_statement = STATEMENT_GSA;
        }
#endif
#if CONFIG_NMEA_STATEMENT_RMC
        else if (strstr(esp_gps->item_str, "RMC")) {
            esp_gps->cur_statement = STATEMENT_RMC;
        }
#endif
#if CONFIG_NMEA_STATEMENT_GSV
        else if (strstr(esp_gps->item_str, "GSV")) {
            esp_gps->cur_statement = STATEMENT_GSV;
        }
#endif
#if CONFIG_NMEA_STATEMENT_GLL
        else if (strstr(esp_gps->item_str, "GLL")) {
            esp_gps->cur_statement = STATEMENT_GLL;
        }
#endif
#if CONFIG_NMEA_STATEMENT_VTG
        else if (strstr(esp_gps->item_str, "VTG")) {
            esp_gps->cur_statement = STATEMENT_VTG;
        }
#endif
        else {
            esp_gps->cur_statement = STATEMENT_UNKNOWN;
        }
        goto out;
    }
    /* Parse each item, depend on the type of the statement */
    if (esp_gps->cur_statement == STATEMENT_UNKNOWN) {
        goto out;
    }
#if CONFIG_NMEA_STATEMENT_GGA
    else if (esp_gps->cur_statement == STATEMENT_GGA) {
        parse_gga(esp_gps);
    }
#endif
#if CONFIG_NMEA_STATEMENT_GSA
    else if (esp_gps->cur_statement == STATEMENT_GSA) {
        parse_gsa(esp_gps);
    }
#endif
#if CONFIG_NMEA_STATEMENT_GSV
    else if (esp_gps->cur_statement == STATEMENT_GSV) {
        parse_gsv(esp_gps);
    }
#endif
#if CONFIG_NMEA_STATEMENT_RMC
    else if (esp_gps->cur_statement == STATEMENT_RMC) {
        parse_rmc(esp_gps);
    }
#endif
#if CONFIG_NMEA_STATEMENT_GLL
    else if (esp_gps->cur_statement == STATEMENT_GLL) {
        parse_gll(esp_gps);
    }
#endif
#if CONFIG_NMEA_STATEMENT_VTG
    else if (esp_gps->cur_statement == STATEMENT_VTG) {
        parse_vtg(esp_gps);
    }
#endif
    else {
        err =  ESP_FAIL;
    }
out:
    return err;
}

/**
 * @brief Parse NMEA statements from GPS receiver
 *
 * @param esp_gps esp_gps_t type object
 * @param data sentencia completa, terminada en '\r' y NUL
 * @return esp_err_t ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t gps_decode(esp_gps_t *esp_gps, const uint8_t *data)
{
    const uint8_t *d = data;
    while (*d) {
        /* Start of a statement */
        if (*d == '$') {
            /* Reset runtime information */
            esp_gps->asterisk = 0;
            esp_gps->item_num = 0;
            esp_gps->item_pos = 0;
            esp_gps->cur_statement = 0;
            esp_gps->crc = 0;
            esp_gps->sat_count = 0;
            esp_gps->sat_num = 0;
            /* Add character to item (bounded — see note at the other write site) */
            if (esp_gps->item_pos < NMEA_MAX_STATEMENT_ITEM_LENGTH - 1) {
                esp_gps->item_str[esp_gps->item_pos++] = *d;
                esp_gps->item_str[esp_gps->item_pos] = '\0';
            }
        }
        /* Detect item separator character */
        else if (*d == ',') {
            /* Parse current item */
            parse_item(esp_gps);
            /* Add character to CRC computation */
            esp_gps->crc ^= (uint8_t)(*d);
            /* Start with next item */
            esp_gps->item_pos = 0;
            esp_gps->item_str[0] = '\0';
            esp_gps->item_num++;
        }
        /* End of CRC computation */
        else if (*d == '*') {
            /* Parse current item */
            parse_item(esp_gps);
            /* Asterisk detected */
            esp_gps->asterisk = 1;
            /* Start with next item */
            esp_gps->item_pos = 0;
            esp_gps->item_str[0] = '\0';
            esp_gps->item_num++;
        }
        /* End of statement */
        else if (*d == '\r') {
            /* Convert received CRC from string (hex) to number */
            uint8_t crc = (uint8_t)strtol(esp_gps->item_str, NULL, 16);
            /* CRC passed */
            if (esp_gps->crc == crc) {
                /* Confirms the current baud rate is correctly decoding the
                 * link, whether or not this particular statement carries a
                 * fix — stops the baud auto-probe from firing just because
                 * the GPS itself has no fix (e.g. RMC status 'V'). */
                esp_gps->last_valid_tick = xTaskGetTickCount();
                esp_gps->baud_locked = true;
                switch (esp_gps->cur_statement) {
#if CONFIG_NMEA_STATEMENT_GGA
                case STATEMENT_GGA:
                    esp_gps->parsed_statement |= 1 << STATEMENT_GGA;
                    break;
#endif
#if CONFIG_NMEA_STATEMENT_GSA
                case STATEMENT_GSA:
                    esp_gps->parsed_statement |= 1 << STATEMENT_GSA;
                    break;
#endif
#if CONFIG_NMEA_STATEMENT_RMC
                case STATEMENT_RMC:
                    esp_gps->parsed_statement |= 1 << STATEMENT_RMC;
                    break;
#endif
#if CONFIG_NMEA_STATEMENT_GSV
                case STATEMENT_GSV:
                    if (esp_gps->sat_num == esp_gps->sat_count) {
                        esp_gps->parsed_statement |= 1 << STATEMENT_GSV;
                    }
                    break;
#endif
#if CONFIG_NMEA_STATEMENT_GLL
                case STATEMENT_GLL:
                    esp_gps->parsed_statement |= 1 << STATEMENT_GLL;
                    break;
#endif
#if CONFIG_NMEA_STATEMENT_VTG
                case STATEMENT_VTG:
                    esp_gps->parsed_statement |= 1 << STATEMENT_VTG;
                    break;
#endif
                default:
                    break;
                }
                /* One update per fix cycle, anchored on RMC.
                 *
                 * Upstream gates this on `(parsed & all_statements) == all_statements`,
                 * but all six statements are enabled in sdkconfig while the Ruptela
                 * only ever sends RMC, GNS and sometimes GGA — so that condition never
                 * becomes true and the event would never fire. Firing on *any* parsed
                 * statement (the previous behaviour) went too far the other way: with a
                 * feed carrying both RMC and GGA it posted twice per second, so every
                 * fix paid for two full 9-tile sweeps and twice the SD reads.
                 *
                 * RMC carries position, speed, course and time — everything the matcher
                 * consumes. The other fields (altitude, satellites) accumulate in the
                 * same struct and ride along with the next post. */
                if (esp_gps->cur_statement == STATEMENT_RMC) {
                    esp_gps->parsed_statement = 0;
                    /* P02a: publicar sólo después de checksum, estructura y
                     * validación de campos.
                     *
                     * Antes se publicaba cualquier RMC con checksum válido, sin
                     * mirar el indicador de validez A/V ni las coordenadas. Un
                     * RMC con status 'V' (receptor sin fix) llega con posición 0
                     * o con la anterior, y aun así disparaba un barrido completo
                     * de 9 tiles y actualizaba la pantalla con un dato que el
                     * propio receptor declara inválido.
                     *
                     * Ahora se exige: status 'A', coordenadas finitas y dentro de
                     * rango. Los rechazos se cuentan para que un log de campo
                     * muestre la diferencia entre «no llega nada» y «llega
                     * inválido», que son fallas distintas. El estado de frescura
                     * y qué mostrar cuando no hay fix válido son de P02b y P03. */
                    if (!esp_gps->parent.valid) {
                        esp_gps->rejected_invalid_fix++;
                    } else if (!isfinite(esp_gps->parent.latitude) ||
                               !isfinite(esp_gps->parent.longitude) ||
                               esp_gps->parent.latitude < -90.0f ||
                               esp_gps->parent.latitude > 90.0f ||
                               esp_gps->parent.longitude < -180.0f ||
                               esp_gps->parent.longitude > 180.0f) {
                        esp_gps->rejected_out_of_range++;
                    } else {
                        /* Send signal to notify that GPS information has been updated */
                        esp_err_t post_err = esp_event_post_to(esp_gps->event_loop_hdl,
                                                              ESP_NMEA_EVENT, GPS_UPDATE,
                                                              &(esp_gps->parent), sizeof(gps_t),
                                                              100 / portTICK_PERIOD_MS);
                        if (post_err != ESP_OK) {
                            /* Cola del event loop llena: el fix se pierde. Contarlo
                             * en lugar de ignorar el retorno, que era lo anterior. */
                            res_metrics_error(RES_ERR_QUEUE_FULL);
                            ESP_LOGW(GPS_TAG, "GPS_UPDATE no encolado: %s",
                                     esp_err_to_name(post_err));
                        }
                    }
                }
            } else {
                ESP_LOGD(GPS_TAG, "CRC Error for statement:%s", (const char *)data);
            }
            /* Unknown statements (the Ruptela `###IMEI...` marker, $GNGNS/$GNGST,
             * binary I/O frames) arrive several times per second. Posting a
             * GPS_UNKNOWN event for each one only floods the event loop (each post
             * can block up to 100 ms) and spams the log — nothing consumes it. So
             * we no longer post it; this removes a big chunk of the per-fix load. */

            /* Close the statement. Parsing state is otherwise only reset on '$'
             * (see the start-of-statement branch), and the Ruptela emits
             * `###IMEI...` lines that carry no '$' at all — they used to inherit
             * everything from the sentence just finished: cur_statement still
             * RMC, asterisk still 1 so the CRC was never recomputed, and item_str
             * still holding the previous checksum digits, which strtol() then read
             * back as the very CRC it was compared against. The check passed
             * against itself and a second identical GPS_UPDATE went out for every
             * fix, doubling the tile sweeps and the SD reads. */
            esp_gps->cur_statement = STATEMENT_UNKNOWN;
            esp_gps->asterisk = 0;
            esp_gps->item_num = 0;
            esp_gps->item_pos = 0;
            esp_gps->item_str[0] = '\0';
            esp_gps->crc = 0;
        }
        /* Other non-space character */
        else {
            if (!(esp_gps->asterisk)) {
                /* Add to CRC */
                esp_gps->crc ^= (uint8_t)(*d);
            }
            /* Add character to item (bounded). An overlong field with no comma —
             * the Ruptela `###IMEI...` line and binary I/O frames — must not
             * overflow the 16-byte item_str and corrupt the struct; that trashed
             * event_loop_hdl and caused the LoadProhibited crash. Excess bytes of
             * such non-NMEA junk are simply dropped. */
            if (esp_gps->item_pos < NMEA_MAX_STATEMENT_ITEM_LENGTH - 1) {
                esp_gps->item_str[esp_gps->item_pos++] = *d;
                esp_gps->item_str[esp_gps->item_pos] = '\0';
            }
        }
        /* Process next character */
        d++;
    }
    return ESP_OK;
}

/* --- Puentes desde los scanners hacia los consumidores registrados ---
 *
 * Sólo reenvían un valor primitivo. Corren en la tarea que drena la UART, así que
 * lo que hagan los consumidores no puede bloquear: la regla es que encolen un
 * mensaje acotado y vuelvan.
 */
static void on_io_ignition(bool ignition_on, void *user_ctx)
{
    esp_gps_t *esp_gps = (esp_gps_t *)user_ctx;
    if (esp_gps->ignition_cb) {
        esp_gps->ignition_cb(esp_gps->signal_ctx, ignition_on);
    }
}

static void on_io_gprs(bool gprs_up, void *user_ctx)
{
    esp_gps_t *esp_gps = (esp_gps_t *)user_ctx;
    if (esp_gps->gprs_cb) {
        esp_gps->gprs_cb(esp_gps->signal_ctx, gprs_up);
    }
}

static void on_io_record(const uint8_t *record, size_t record_len, void *user_ctx)
{
    esp_gps_t *esp_gps = (esp_gps_t *)user_ctx;
    if (esp_gps->record_cb) {
        esp_gps->record_cb(esp_gps->signal_ctx, record, record_len);
    }
}

static void on_imei_found(void *ctx, const char *imei, size_t len)
{
    (void)len;
    esp_gps_t *esp_gps = (esp_gps_t *)ctx;
    if (esp_gps->imei_cb) {
        esp_gps->imei_cb(esp_gps->signal_ctx, imei);
    }
}

/**
 * @brief Entrega de una sentencia completa y con checksum válido.
 *
 * La invoca el framer compartido. El texto llega desde '$' hasta el segundo
 * dígito del checksum, sin CR/LF. Se le agrega un '\r' porque la máquina de
 * estados de `gps_decode` cierra la sentencia en ese carácter; así el parseo de
 * campos queda idéntico a la línea base y este cambio se limita al transporte.
 *
 * El checksum se verifica dos veces: en el framer, que decide si la sentencia
 * existe, y en `gps_decode`, que lo recalcula mientras extrae los campos. Es a
 * propósito: mantiene el parseo de campos sin tocar. La verificación redundante
 * se saca cuando el parseo de campos se extraiga, en P02b.
 */
static void on_nmea_sentence(void *ctx, const char *sentence, size_t len)
{
    esp_gps_t *esp_gps = (esp_gps_t *)ctx;

    if (len + 2 > sizeof(esp_gps->sentence_buf)) {
        res_metrics_error(RES_ERR_UART_FRAMING);
        return;
    }
    memcpy(esp_gps->sentence_buf, sentence, len);
    esp_gps->sentence_buf[len] = '\r';
    esp_gps->sentence_buf[len + 1] = '\0';

    if (gps_decode(esp_gps, esp_gps->sentence_buf) != ESP_OK) {
        ESP_LOGW(GPS_TAG, "GPS decode line failed");
    }
}

/**
 * @brief Drena todos los bytes disponibles de la UART.
 *
 * Reemplaza el manejo por detección de patrón '\n'. Motivo: el enlace del
 * Ruptela es un canal transparente que además transporta records binarios y el
 * marcador `###IMEI`. Con detección de patrón, un 0x0A dentro de un payload
 * binario disparaba un evento espurio que partía el frame, y la lectura
 * terminaba en NUL, de modo que un 0x00 descartaba el resto del bloque.
 *
 * Ahora se lee lo que haya, sin esperar ningún delimitador, y las fronteras las
 * repone el framer por estructura. Se registra además el intervalo entre
 * drenajes, que es la métrica que el plan pide con p99 <25 ms.
 */
static void drain_uart(esp_gps_t *esp_gps)
{
    res_metrics_mark_interval(RES_CH_UART_DRAIN);

    while (1) {
        size_t avail = 0;
        if (uart_get_buffered_data_len(esp_gps->uart_port, &avail) != ESP_OK) {
            res_metrics_error(RES_ERR_UART_FRAMING);
            return;
        }
        if (avail == 0) {
            return;
        }
        size_t chunk = avail;
        if (chunk > NMEA_PARSER_RUNTIME_BUFFER_SIZE - 1) {
            chunk = NMEA_PARSER_RUNTIME_BUFFER_SIZE - 1;
        }
        /* Timeout 0: no bloquear nunca dentro del drenaje. Los bytes ya están en
         * el ring, así que no hay nada que esperar. */
        int read_len = uart_read_bytes(esp_gps->uart_port, esp_gps->buffer, chunk, 0);
        if (read_len <= 0) {
            return;
        }
        /* Sin terminador NUL y sin buscar delimitadores: bytes crudos a los tres
         * consumidores. Cada uno repone sus propias fronteras —el framer por
         * estructura de sentencia, el parser de IO por longitud declarada, el
         * scanner de IMEI por prefijo— y ninguno depende de los otros.
         *
         * Notar que el framer llama a on_nmea_sentence, que copia la sentencia a
         * `sentence_buf` en lugar de reusar `buffer`: los bytes crudos siguen
         * siendo recorridos por los consumidores de abajo. */
        nmea_framer_feed(&esp_gps->framer, esp_gps->buffer, (size_t)read_len);
        ruptela_io_parser_feed(&esp_gps->io_parser, esp_gps->buffer, (size_t)read_len);
        imei_scanner_feed(&esp_gps->imei_scanner, esp_gps->buffer, (size_t)read_len);
    }
}

/**
 * @brief NMEA Parser Task Entry
 *
 * @param arg argument
 */
static void nmea_parser_task_entry(void *arg)
{
    esp_gps_t *esp_gps = (esp_gps_t *)arg;
    uart_event_t event;
    res_metrics_watch_task("nmea", NULL);
    while (1) {
        if (xQueueReceive(esp_gps->event_queue, &event, pdMS_TO_TICKS(200))) {
            switch (event.type) {
            case UART_DATA:
                /* P02a: antes esta rama estaba vacía y la lectura dependía por
                 * completo de UART_PATTERN_DET con '\n'. Ahora es el camino
                 * normal de drenaje. */
                drain_uart(esp_gps);
                break;
            case UART_FIFO_OVF:
                ESP_LOGW(GPS_TAG, "HW FIFO Overflow");
                res_metrics_error(RES_ERR_UART_OVERFLOW);
                uart_flush(esp_gps->uart_port);
                xQueueReset(esp_gps->event_queue);
                /* Se perdieron bytes: lo que venga no continúa la sentencia a
                 * medio armar. Descartarla en lugar de pegar dos mitades que no
                 * son consecutivas. */
                nmea_framer_discard_partial(&esp_gps->framer);
                break;
            case UART_BUFFER_FULL:
                ESP_LOGW(GPS_TAG, "Ring Buffer Full");
                res_metrics_error(RES_ERR_UART_OVERFLOW);
                uart_flush(esp_gps->uart_port);
                xQueueReset(esp_gps->event_queue);
                nmea_framer_discard_partial(&esp_gps->framer);
                break;
            case UART_BREAK:
                ESP_LOGW(GPS_TAG, "Rx Break");
                res_metrics_error(RES_ERR_UART_FRAMING);
                nmea_framer_discard_partial(&esp_gps->framer);
                break;
            case UART_PARITY_ERR:
                ESP_LOGE(GPS_TAG, "Parity Error");
                res_metrics_error(RES_ERR_UART_FRAMING);
                break;
            case UART_FRAME_ERR:
                ESP_LOGE(GPS_TAG, "Frame Error");
                res_metrics_error(RES_ERR_UART_FRAMING);
                break;
            default:
                ESP_LOGW(GPS_TAG, "unknown uart event type: %d", event.type);
                break;
            }
        } else {
            /* Timeout de cola: drenar igual. Un evento perdido o una cola llena
             * no puede dejar bytes indefinidamente en el ring. */
            drain_uart(esp_gps);
        }
        /* Baud auto-detect. Checked every iteration, not only on a queue
         * timeout: at the wrong baud rate the UART still produces a steady
         * stream of garbage events (framing errors, spurious pattern
         * matches), so the queue rarely actually times out. */
        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - esp_gps->last_valid_tick;
        if (esp_gps->baud_locked && elapsed > pdMS_TO_TICKS(NMEA_BAUD_RELOCK_MS)) {
            esp_gps->baud_locked = false;
            ESP_LOGW(GPS_TAG, "NMEA link stale, resuming baud probe");
        }
        if (!esp_gps->baud_locked && elapsed > pdMS_TO_TICKS(NMEA_BAUD_PROBE_MS)) {
            esp_gps->baud_idx = (esp_gps->baud_idx + 1) % (sizeof(nmea_baud_candidates) / sizeof(nmea_baud_candidates[0]));
            nmea_parser_set_baud(esp_gps, nmea_baud_candidates[esp_gps->baud_idx]);
            esp_gps->last_valid_tick = xTaskGetTickCount();
            ESP_LOGW(GPS_TAG, "no valid NMEA, probing %" PRIu32 " baud", esp_gps->baud_rate);
        }
        /* Drive the event loop */
        esp_event_loop_run(esp_gps->event_loop_hdl, pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}

/**
 * @brief Init NMEA Parser
 *
 * @param config Configuration of NMEA Parser
 * @return nmea_parser_handle_t handle of nmea_parser
 */
nmea_parser_handle_t nmea_parser_init(const nmea_parser_config_t *config)
{
    esp_gps_t *esp_gps = calloc(1, sizeof(esp_gps_t));
    if (!esp_gps) {
        ESP_LOGE(GPS_TAG, "calloc memory for esp_fps failed");
        goto err_gps;
    }
    esp_gps->buffer = calloc(1, NMEA_PARSER_RUNTIME_BUFFER_SIZE);
    if (!esp_gps->buffer) {
        ESP_LOGE(GPS_TAG, "calloc memory for runtime buffer failed");
        goto err_buffer;
    }
#if CONFIG_NMEA_STATEMENT_GSA
    esp_gps->all_statements |= (1 << STATEMENT_GSA);
#endif
#if CONFIG_NMEA_STATEMENT_GSV
    esp_gps->all_statements |= (1 << STATEMENT_GSV);
#endif
#if CONFIG_NMEA_STATEMENT_GGA
    esp_gps->all_statements |= (1 << STATEMENT_GGA);
#endif
#if CONFIG_NMEA_STATEMENT_RMC
    esp_gps->all_statements |= (1 << STATEMENT_RMC);
#endif
#if CONFIG_NMEA_STATEMENT_GLL
    esp_gps->all_statements |= (1 << STATEMENT_GLL);
#endif
#if CONFIG_NMEA_STATEMENT_VTG
    esp_gps->all_statements |= (1 << STATEMENT_VTG);
#endif
    /* Set attributes */
    esp_gps->uart_port = config->uart.uart_port;
    esp_gps->all_statements &= 0xFE;
    esp_gps->baud_rate = config->uart.baud_rate;
    esp_gps->baud_locked = false;
    esp_gps->last_valid_tick = xTaskGetTickCount();
    esp_gps->baud_idx = 0;
    for (size_t i = 0; i < sizeof(nmea_baud_candidates) / sizeof(nmea_baud_candidates[0]); i++) {
        if (nmea_baud_candidates[i] == config->uart.baud_rate) {
            esp_gps->baud_idx = i;
            break;
        }
    }
    /* Install UART friver */
    uart_config_t uart_config = {
        .baud_rate = config->uart.baud_rate,
        .data_bits = config->uart.data_bits,
        .parity = config->uart.parity,
        .stop_bits = config->uart.stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(esp_gps->uart_port, CONFIG_NMEA_PARSER_RING_BUFFER_SIZE, 0,
                            config->uart.event_queue_size, &esp_gps->event_queue, 0) != ESP_OK) {
        ESP_LOGE(GPS_TAG, "install uart driver failed");
        goto err_uart_install;
    }
    if (uart_param_config(esp_gps->uart_port, &uart_config) != ESP_OK) {
        ESP_LOGE(GPS_TAG, "config uart parameter failed");
        goto err_uart_config;
    }
    if (uart_set_pin(esp_gps->uart_port, UART_PIN_NO_CHANGE, config->uart.rx_pin,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(GPS_TAG, "config uart gpio failed");
        goto err_uart_config;
    }
    /* P02a: ya no se habilita la detección de patrón '\n'. El enlace transporta
     * records binarios y el marcador `###IMEI`, así que un 0x0A dentro de un
     * payload disparaba eventos espurios que partían el frame. Las fronteras las
     * repone el framer por estructura. */
    esp_err_t flush_err = uart_flush(esp_gps->uart_port);
    if (flush_err != ESP_OK) {
        ESP_LOGW(GPS_TAG, "uart_flush inicial falló: %s", esp_err_to_name(flush_err));
    }
    nmea_framer_init(&esp_gps->framer, on_nmea_sentence, esp_gps);
    /* Scanners del canal transparente. Se inicializan siempre: cuestan un buffer
     * de 257 B y otro de 24 B dentro de la struct, y así el enlace queda
     * observado desde el primer byte aunque todavía no haya consumidor
     * registrado. */
    ruptela_io_parser_init(&esp_gps->io_parser, on_io_ignition, esp_gps);
    ruptela_io_parser_set_gprs_callback(&esp_gps->io_parser, on_io_gprs);
    ruptela_io_parser_set_record_callback(&esp_gps->io_parser, on_io_record);
    imei_scanner_init(&esp_gps->imei_scanner, on_imei_found, esp_gps);
    /* Create Event loop */
    esp_event_loop_args_t loop_args = {
        .queue_size = NMEA_EVENT_LOOP_QUEUE_SIZE,
        .task_name = NULL
    };
    if (esp_event_loop_create(&loop_args, &esp_gps->event_loop_hdl) != ESP_OK) {
        ESP_LOGE(GPS_TAG, "create event loop failed");
        goto err_eloop;
    }
    /* Create NMEA Parser task */
    BaseType_t err = xTaskCreate(
                         nmea_parser_task_entry,
                         "nmea_parser",
                         CONFIG_NMEA_PARSER_TASK_STACK_SIZE,
                         esp_gps,
                         CONFIG_NMEA_PARSER_TASK_PRIORITY,
                         &esp_gps->tsk_hdl);
    if (err != pdTRUE) {
        ESP_LOGE(GPS_TAG, "create NMEA Parser task failed");
        goto err_task_create;
    }
    ESP_LOGI(GPS_TAG, "NMEA Parser init OK");
    return esp_gps;
    /*Error Handling*/
err_task_create:
    esp_event_loop_delete(esp_gps->event_loop_hdl);
err_eloop:
err_uart_install:
    uart_driver_delete(esp_gps->uart_port);
err_uart_config:
err_buffer:
    free(esp_gps->buffer);
err_gps:
    free(esp_gps);
    return NULL;
}

/**
 * @brief Deinit NMEA Parser
 *
 * @param nmea_hdl handle of NMEA parser
 * @return esp_err_t ESP_OK on success,ESP_FAIL on error
 */
esp_err_t nmea_parser_deinit(nmea_parser_handle_t nmea_hdl)
{
    esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
    vTaskDelete(esp_gps->tsk_hdl);
    esp_event_loop_delete(esp_gps->event_loop_hdl);
    esp_err_t err = uart_driver_delete(esp_gps->uart_port);
    free(esp_gps->buffer);
    free(esp_gps);
    return err;
}

/**
 * @brief Add user defined handler for NMEA parser
 *
 * @param nmea_hdl handle of NMEA parser
 * @param event_handler user defined event handler
 * @param handler_args handler specific arguments
 * @return esp_err_t
 *  - ESP_OK: Success
 *  - ESP_ERR_NO_MEM: Cannot allocate memory for the handler
 *  - ESP_ERR_INVALIG_ARG: Invalid combination of event base and event id
 *  - Others: Fail
 */
esp_err_t nmea_parser_add_handler(nmea_parser_handle_t nmea_hdl, esp_event_handler_t event_handler, void *handler_args)
{
    esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
    return esp_event_handler_register_with(esp_gps->event_loop_hdl, ESP_NMEA_EVENT, ESP_EVENT_ANY_ID,
                                           event_handler, handler_args);
}

/**
 * @brief Remove user defined handler for NMEA parser
 *
 * @param nmea_hdl handle of NMEA parser
 * @param event_handler user defined event handler
 * @return esp_err_t
 *  - ESP_OK: Success
 *  - ESP_ERR_INVALIG_ARG: Invalid combination of event base and event id
 *  - Others: Fail
 */
esp_err_t nmea_parser_remove_handler(nmea_parser_handle_t nmea_hdl, esp_event_handler_t event_handler)
{
    esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
    return esp_event_handler_unregister_with(esp_gps->event_loop_hdl, ESP_NMEA_EVENT, ESP_EVENT_ANY_ID, event_handler);
}

esp_err_t nmea_parser_set_baud(nmea_parser_handle_t nmea_hdl, uint32_t baud_rate)
{
    esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
    esp_err_t err = uart_set_baudrate(esp_gps->uart_port, baud_rate);
    if (err != ESP_OK) {
        return err;
    }
    esp_gps->baud_rate = baud_rate;
    err = uart_flush_input(esp_gps->uart_port);
    if (err != ESP_OK) {
        ESP_LOGW(GPS_TAG, "uart_flush_input falló al cambiar baud: %s", esp_err_to_name(err));
    }
    xQueueReset(esp_gps->event_queue);
    /* Discard any statement fragment left over from the previous baud rate */
    esp_gps->item_pos = 0;
    esp_gps->item_num = 0;
    esp_gps->asterisk = 0;
    esp_gps->crc = 0;
    esp_gps->parsed_statement = 0;
    /* Lo acumulado a otro baud es basura: se descarta la sentencia parcial y se
     * limpian los contadores, que corresponden al enlace anterior. */
    nmea_framer_reset(&esp_gps->framer);
    return ESP_OK;
}

uint32_t nmea_parser_get_baud(nmea_parser_handle_t nmea_hdl)
{
    esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
    return esp_gps->baud_rate;
}

esp_err_t nmea_parser_set_signal_handlers(nmea_parser_handle_t nmea_hdl,
                                         nmea_ignition_cb_t ignition_cb,
                                         nmea_gprs_cb_t gprs_cb,
                                         nmea_imei_cb_t imei_cb,
                                         void *ctx)
{
    if (!nmea_hdl) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
    esp_gps->signal_ctx = ctx;
    esp_gps->ignition_cb = ignition_cb;
    esp_gps->gprs_cb = gprs_cb;
    esp_gps->imei_cb = imei_cb;
    return ESP_OK;
}

esp_err_t nmea_parser_set_record_handler(nmea_parser_handle_t nmea_hdl,
                                         nmea_record_cb_t record_cb)
{
    if (!nmea_hdl) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
    esp_gps->record_cb = record_cb;
    return ESP_OK;
}