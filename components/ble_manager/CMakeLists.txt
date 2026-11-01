idf_component_register(SRCS "ble_profiles.c" "ble_esp.c" "ble_tools.c"
                       INCLUDE_DIRS "include"
                       REQUIRES tool_registry
                       PRIV_REQUIRES bt agent_core config_manager event_engine network_manager peripheral_registry esp_timer)
