# Geometría pura: la fuente de candidatos se inyecta, así que este componente no
# sabe de archivos ni de tarjeta y compila igual en el host.
idf_component_register(SRCS "map_match.c"
                       INCLUDE_DIRS "include")
