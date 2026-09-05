# Dueño único de la radio y de la configuración de red. Envuelve esp_wifi/esp_netif,
# así que NO se puede probar en el host: su verificación necesita la placa y un AP.
idf_component_register(SRCS "connectivity_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash)
