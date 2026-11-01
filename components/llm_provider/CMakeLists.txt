idf_component_register(SRCS "llm_common.c" "llm_conv.c" "provider_openai.c" "provider_anthropic.c" INCLUDE_DIRS "include" REQUIRES lla_common tool_registry)
