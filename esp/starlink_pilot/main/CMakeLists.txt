idf_component_register(
    SRCS
        "app_main.c"
        "nmea_parser.c"
        "telematics.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_event esp_wifi nvs_flash esp_netif lwip ruptela_framing
)
