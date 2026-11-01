# CLAUDE.md — working on llmagent

## Commands
- Host tests (always run before committing):
  ```
  cmake -S tests/host -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure
  ```
- Firmware: `. ~/esp/esp-idf-v5.5.6/export.sh && idf.py build` (target esp32c5, ESP-IDF v5.5.6 pinned)
- Flash and monitor: `idf.py -p /dev/cu.usbserial-110 flash monitor`
- Live protocol test: `./build-host/live_agent openai http://127.0.0.1:8080/v1 <model> 1`

## Layout rules
- Portable code (no ESP-IDF includes) lives in:
  - `lla_common/lla_json.c`, `tool_registry/`, `llm_provider/`, `agent_core/agent.c`
  - `peripheral_registry/`, `event_engine/{events,rules}.c`, `config_manager/config_core.c`
  - `hardware_hal/hal.c`, `ble_manager/ble_profiles.c`

  ESP-only files end in `_esp.c`. The host build globs these directories and
  excludes `*_esp.c`.
- Hardware is reached only through `hal()`.
- The agent addresses devices by name through `periph_*`. Never add a tool
  that takes a raw pin, address, or register from the model unless a local
  allowlist gates it.
- No heap allocation inside the agent loop. Rare paths may use temporary
  malloc; prefer PSRAM.

## Safety invariants (tests enforce them)
- Never execute a tool call from a truncated or invalid response, or from model prose.
- Never automatically repeat a NON_IDEMPOTENT call or an UNKNOWN outcome.
- Outputs reach their safe state before networking starts and on shutdown.
- Secrets live only in NVS namespace `llasec`. Never log them; use `cfg_redact()`.

## Conventions
- C17, 4-space indent, 120 columns, `snake_case`, module prefixes
  (`lla_`, `llm_`, `tool_`, `periph_`, `ble_mgr_`).
- Verify ESP-IDF APIs against `~/esp/esp-idf-v5.5.6/components` before using them.
- Document numbers in docs/PERFORMANCE.md only after measuring them.
- Commits: descriptive messages, no AI attribution trailers.
