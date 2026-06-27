idf_component_register(SRCS "map_sync.c"
                       INCLUDE_DIRS "."
                       REQUIRES net_link esp_http_client esp-tls json sd_card nvs_flash mbedtls)

# Cert self-signed del servidor de prueba, embebido solo si se pide (Kconfig).
# Generar con:  python map_test_server.py --print-cert > components/map_sync/test_server_cert.pem
if(CONFIG_MAP_SYNC_TEST_CERT)
    target_add_binary_data(${COMPONENT_LIB} "test_server_cert.pem" TEXT)
endif()
