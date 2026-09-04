idf_component_register(
    SRCS
        "app_main.c"
        "nmea_parser.c"
        "ruptela_io_parser.c"
        "ruptela_proto.c"
        "telematics.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_event esp_wifi nvs_flash esp_netif lwip
)
