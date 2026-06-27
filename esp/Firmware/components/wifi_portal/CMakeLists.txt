idf_component_register(SRCS "wifi_portal.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event esp_http_server lwip json net_link map_sync)
