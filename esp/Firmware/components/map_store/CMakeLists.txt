# Almacenamiento y caché de tiles: dueño de la generación y del ciclo de vida de
# los bytes que consume el matcher.
idf_component_register(SRCS "map_store.c"
                            "tile_cache.c"
                       INCLUDE_DIRS "include"
                       REQUIRES map_match sd_card map_capsule
                       PRIV_REQUIRES esp_psram)
