set(reqs ble_manager agent_core config_manager network_manager peripheral_registry event_engine console app_update esp_timer)
idf_component_register(SRCS "console_esp.c" INCLUDE_DIRS "include" REQUIRES tool_registry PRIV_REQUIRES ${reqs})
