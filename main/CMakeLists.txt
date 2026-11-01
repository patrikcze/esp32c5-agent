set(reqs ble_manager agent_core config_manager network_manager peripheral_registry hardware_hal event_engine lla_console
         system_tools tool_registry app_update esp_pm esp_psram spi_flash bootloader_support)
idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "default_profile.json"
                       PRIV_REQUIRES ${reqs})
