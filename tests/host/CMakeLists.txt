# Host-side unit, protocol and integration tests for the portable llmagent core.
# Usage:  cmake -S tests/host -B build-host && cmake --build build-host && ctest --test-dir build-host
cmake_minimum_required(VERSION 3.16)
project(llmagent_host_tests C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(C ${ROOT}/components)

option(LLA_SANITIZE "Build with ASan/UBSan" ON)

set(PORTABLE_SRCS
    ${C}/lla_common/lla_json.c
    ${C}/tool_registry/tool_registry.c
    ${C}/llm_provider/llm_common.c
    ${C}/llm_provider/llm_conv.c
    ${C}/llm_provider/provider_openai.c
    ${C}/llm_provider/provider_anthropic.c
    ${C}/agent_core/agent.c
)
file(GLOB EXTRA_PORTABLE ${C}/hardware_hal/hal.c ${C}/peripheral_registry/*.c ${C}/event_engine/*.c ${C}/config_manager/config_core.c ${C}/ble_manager/ble_profiles.c)
list(FILTER EXTRA_PORTABLE EXCLUDE REGEX ".*_esp\\.c$")

file(GLOB TEST_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.c)

add_executable(llmagent_tests ${PORTABLE_SRCS} ${EXTRA_PORTABLE} ${TEST_SRCS})
target_include_directories(llmagent_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${C}/lla_common/include ${C}/tool_registry/include ${C}/llm_provider/include
    ${C}/agent_core/include ${C}/peripheral_registry/include ${C}/event_engine/include
    ${C}/config_manager/include ${C}/hardware_hal/include ${C}/ble_manager/include)
target_compile_definitions(llmagent_tests PRIVATE FIXTURE_DIR="${ROOT}/tests/protocol/fixtures" LLA_HOST_TEST=1)
target_compile_options(llmagent_tests PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -Wshadow -Wformat=2)
target_link_libraries(llmagent_tests PRIVATE m)
if(LLA_SANITIZE)
    target_compile_options(llmagent_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(llmagent_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
foreach(s json registry openai anthropic conv agent periph rules events config)
    add_test(NAME ${s} COMMAND llmagent_tests ${s})
endforeach()

# Live protocol test against a running server (not part of ctest):
#   ./live_agent openai http://127.0.0.1:8080/v1 <model> [stream 0|1]
add_executable(live_agent ${ROOT}/tests/integration/live_agent.c ${PORTABLE_SRCS})
target_include_directories(live_agent PRIVATE ${C}/lla_common/include ${C}/tool_registry/include
    ${C}/llm_provider/include ${C}/agent_core/include)
target_compile_options(live_agent PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(LLA_SANITIZE)
    target_compile_options(live_agent PRIVATE -fsanitize=address,undefined)
    target_link_options(live_agent PRIVATE -fsanitize=address,undefined)
endif()
