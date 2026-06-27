idf_component_register(SRCS "net_link.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash json sd_card)
