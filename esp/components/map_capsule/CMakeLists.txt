# Contenedor de mapas consultable por rangos. C puro: el lector se inyecta, así que
# el mismo código corre contra la tarjeta en el dispositivo y contra un archivo en
# el host. Se prueba en el host.
idf_component_register(SRCS "map_capsule.c"
                            "map_installer.c"
                       INCLUDE_DIRS "include")
