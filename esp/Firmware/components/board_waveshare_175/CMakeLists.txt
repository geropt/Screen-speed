idf_component_register(SRCS "board_waveshare_175.c"
                            "board_i2c.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_lcd esp_lcd_sh8601
                       PRIV_REQUIRES esp_psram spi_flash)
