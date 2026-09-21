# Componente compartido por el HUD (esp/Firmware) y el piloto
# (esp/starlink_pilot). Todo lo que vive acá es C puro, sin dependencias de
# ESP-IDF, para que la misma implementación se compile en los dos targets y se
# pruebe en el host. No agregar acá nada que necesite FreeRTOS, drivers o logs.
idf_component_register(SRCS "ruptela_io_parser.c"
                            "ruptela_proto.c"
                            "nmea_framer.c"
                            "imei_scanner.c"
                       INCLUDE_DIRS "include")
