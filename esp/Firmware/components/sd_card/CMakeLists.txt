idf_component_register(SRCS "sd_manager.c" "sd_test_io.c"
                       INCLUDE_DIRS "."
                       REQUIRES fatfs esp_adc
                       WHOLE_ARCHIVE)
