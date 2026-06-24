set(srcs "offline_maps.c" nmea_parser.c "buttons.c" "diag_log.c" "splash.c" "mykeego_logo.c")

# NOTE: this glob is evaluated at CMake configure time. If EEZ Studio generates a
# new UI/font source, do a Full Clean (or reconfigure) so it gets picked up.
# (CONFIGURE_DEPENDS is not usable here: ESP-IDF runs this file in script mode.)
file(GLOB_RECURSE UI_SRCS "waveshare_amoled_lcd_port.cpp" "vars.c" "dynamic.c" "ui/*.c" "ui/*.cpp")

idf_component_register(SRCS ${srcs} ${UI_SRCS}
                       INCLUDE_DIRS "." "ui")
                    #    REQUIRES sd_card tile_reader SensorLib esp_lcd_sh8601
                    #    WHOLE_ARCHIVE)


# Only apply to eez-flow.cpp
set_source_files_properties(ui/eez-flow.cpp PROPERTIES COMPILE_FLAGS "-Wno-error=dangling-pointer")

if(NOT CONFIG_SOC_SDMMC_HOST_SUPPORTED)
    fail_at_build_time(sdmmc ""
                             "Only ESP32 and ESP32-S3 targets are supported."
                             "Please refer README.md for more details")
endif()
