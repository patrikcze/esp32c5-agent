idf_component_register(SRCS "events.c" "rules.c" "event_task_esp.c" INCLUDE_DIRS "include" REQUIRES lla_common PRIV_REQUIRES peripheral_registry config_manager esp_system)
