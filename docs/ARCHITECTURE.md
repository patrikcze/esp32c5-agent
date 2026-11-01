# Architecture

## Layers

```
 ┌────────────────────────── console (UART REPL) ── BLE request characteristic ─┐
 │                                                                              │
 │   agent_service (task "agent", prio 4)          event_engine (task "events", │
 │     └ agent_core: bounded state machine           prio 6, 50 ms tick, TWDT)  │
 │         ├ llm_provider: openai | anthropic        ├ debounce, sampling       │
 │         │     └ llm_transport ── network_manager  ├ deterministic rules      │
 │         └ tool_registry + policy                  ├ actuator max-on watchdog │
 │               ├ system_tools                      └ triggers → agent jobs    │
 │               ├ periph_tools ── peripheral_registry ── hal ── ESP drivers    │
 │               └ ble_tools ──── ble_manager (NimBLE host task)                │
 │   config_manager (NVS: llacfg + llasec, write budget)                        │
 └──────────────────────────────────────────────────────────────────────────────┘
```

| Layer (brief) | Component(s) | Portable? |
|---|---|---|
| A Hardware abstraction | `hardware_hal` (vtable + ESP impl), `peripheral_registry` | registry & tools yes, `hal_esp.c` no |
| B Networking | `network_manager` (Wi-Fi, SNTP, HTTPS transport, OTA) | no |
| C LLM providers | `llm_provider` (interface, conversation, OpenAI, Anthropic) | yes |
| D Agent runtime | `agent_core` (`agent.c` portable, `agent_service_esp.c`) | core yes |
| E Tools | `tool_registry`, `system_tools`, `periph_tools`, `ble_tools` | registry yes |
| F Memory/config | `config_manager` (`config_core.c` portable, NVS backend) | core yes |
| G Security/safety | spread: policy (E), pin/actuator safety (A), TLS (B), secrets (F) | — |
| Autonomy | `event_engine` (`events.c`, `rules.c` portable, task ESP) | logic yes |
| UI | `lla_console` | no |

"Portable" code builds with `tests/host/CMakeLists.txt` against
`lla_port.h` and the mock HAL, so it is unit-tested on the host.

Deviations from the proposed tree in the brief:

- `tool_policy` is folded into `tool_registry`. Policy is just a check
  inside `invoke`, so a separate component would add a dependency edge and
  nothing else.
- `provider_openai` and `provider_anthropic` are files inside `llm_provider`.
  They share the parser driver and the conversation store. A new provider
  is one file plus one table entry.
- `security` is not a component. Each control lives where it is enforced.
  [SECURITY.md](SECURITY.md) maps every control to its file.
- The console component is called `lla_console`, because `console` would
  shadow ESP-IDF's own component.
- There is no `agent_memory` component. Working memory is the conversation
  arena in `llm_provider`, and persistent preferences (system prompt,
  budgets, policy) live in `config_manager`.

## Tasks

| Task | Prio | Stack | Purpose |
|---|---|---|---|
| `events` | 6 | 4096 | local control loop, never touches the network |
| `agent` | 4 | 12288 | one agent run at a time (TLS handshakes need the stack) |
| `console_repl` | 2 | 6144 | command parsing; agent requests are queued, not run inline |
| `nimble_host` | IDF | IDF | BLE host; public BLE calls wait on a semaphore |
| ESP-IDF tasks | — | — | Wi-Fi, lwIP, timers |

The design adds only three tasks of its own. ISRs (GPIO edges) post a device
index to a queue. All other work happens in the `events` task.

## Memory budget (measured on DevKitC-1, see PERFORMANCE.md)

- **Agent working set**, allocated once at start:
  - with PSRAM: 32 KB request + 16 KB response/SSE line + 16 KB context + 1024 tokens (10 KB), all in PSRAM
  - without PSRAM: 12 / 8 / 6 KB + 512 tokens, from Kconfig
- **`agent_t`**: 8.8 KB static. It holds the response, up to 4 tool calls of
  512 B arguments each, and 4 results of 768 B each.
- **Steady state**: no allocation in the agent loop. mbedTLS and
  esp_http_client allocate per connection. With PSRAM those allocations go
  there; see `sdkconfig.defaults`.
- **Rare operations** (profile load, `/models`, CA paste) allocate temporary
  scratch memory and free it afterwards.

## Data flow of one request

1. The console (or a trigger, or the BLE characteristic) queues a job. The
   queue holds 2 jobs; when it is full, new jobs are dropped and the drop is logged.
2. `agent_run`: `CONNECTING` waits for Wi-Fi. The user message is added to the arena.
3. `REQUESTING_MODEL`: the provider builds the body into the request buffer.
   If the body does not fit, older runs are evicted and the build is retried.
4. The transport streams the body into the parser. The parser accumulates
   SSE lines in the bounded response buffer and parses each event with the
   zero-allocation tokenizer.
5. `PARSING_RESPONSE`: the parser checks for a terminal marker, a consistent
   finish reason, complete tool calls, unique IDs, and arguments that form a
   JSON object.
6. For each call: `VALIDATING_TOOL` (budget, unknown tool, ledger),
   `EXECUTING_TOOL` (policy, argument schema, exec), then `VERIFYING`
   (postcondition readback).
7. `SUBMITTING_RESULT`: the assistant turn and its results are appended,
   and the loop returns to step 3.
