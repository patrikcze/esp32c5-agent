idf_component_register(SRCS "agent.c" "agent_service_esp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES llm_provider tool_registry lla_common
                       PRIV_REQUIRES config_manager network_manager esp_psram esp_timer)
