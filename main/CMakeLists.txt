idf_component_register(SRCS "pdm_mic_volume.c"
                     PRIV_REQUIRES esp_driver_i2s esp_driver_spi esp_lcd
                     INCLUDE_DIRS)