idf_component_register(SRCS "hal.c" "hal_esp.c" INCLUDE_DIRS "include" PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_uart esp_driver_rmt esp_driver_tsens esp_adc)
