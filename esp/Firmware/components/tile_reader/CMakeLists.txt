idf_component_register(SRCS "tile_cache.c" "tile_reader.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sd_card
                       WHOLE_ARCHIVE)
