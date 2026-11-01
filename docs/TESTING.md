# Testing

## 1. Host tests (no hardware)

```bash
cmake -S tests/host -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

The suite runs 513 checks across 10 test groups, compiled with
`-Wall -Wextra -Werror` and AddressSanitizer + UndefinedBehaviorSanitizer.
The `-DLLA_SANITIZE=OFF` option turns the sanitizers off.

| Suite | Covers |
|---|---|
| json | tokenizer (truncation, invalid input, depth, unicode), writer overflow |
| registry | schema validation (types, ranges, enum, unknown/duplicate keys), policy, schema emission, result truncation |
| openai | fixtures: tool call, multiple calls, text, invalid JSON, `finish_reason` quirks, missing id, prose tool call, bad args, HTTP 429/500, streaming at chunk sizes 1/3/17/64, truncated stream, `length`, server ignoring `stream`, too-large body |
| anthropic | fixtures: tool_use, multiple, text, streaming at 4 chunk sizes, truncated, unclosed block, in-stream overloaded error, 429 |
| conv | arena records, whole-run eviction, rollback |
| agent | multi-step workflow, unknown tool/invalid args, 429 + Retry-After, 500 exhaustion, 401 fatal, transport failures (TLS not retried), truncated never executes, malformed recovers, loop detection, turn budget, **lost response no replay**, **unknown outcome not retried**, cancel (in tool and in transport), deadline, per-source permissions, network unavailable, Anthropic end-to-end (non-stream and stream), context eviction |
| periph | profile validation (13 invalid cases), shipped default profile, actuation policy, locks, active-low, PWM limits, RGB brightness cap, max-on watchdog, NTC/linear/SHT3x/chip-temp sensors, bus authorisation, all hardware tools through the registry |
| rules | hysteresis, safety lock vs agent, stale-sensor fail-safe and recovery, validation, offline operation |
| events | queue bound/drop/dedup, debounce, trigger rate limit |
| config | unchanged-write skip, hourly write budget, redaction, factory reset |

Mocks: transport (scripted steps, `tests/host/test_agent.c`), HAL, NVS
(`test_hw.c`), and clock (`port_host.c`: sleeps advance a mock clock, so
backoff tests run instantly). Wi-Fi and BLE logic that needs the radio has no
host test; it is covered by the hardware checklist.

## 2. Live protocol test (host to a real server, no hardware)

```bash
./build-host/live_agent openai http://127.0.0.1:8080/v1 <model> 1
```

This runs the real agent and parsers against a server with mock tools. It
passes when the fan workflow completes and the autonomous-source run
performs no actuation. It is protocol validation, not hardware validation.

## 3. Hardware checklist

See [tests/hardware/CHECKLIST.md](../tests/hardware/CHECKLIST.md). Record the date, firmware
commit, and result of each item.

## Verification status and gaps (2026-11-01)

| # | Requirement | Status |
|---|---|---|
| 1 | Connect to Wi-Fi | ⏳ not run: no credentials provided yet |
| 2 | Authenticate to an LLM | ⏳ device side not run; host live test PASS (llama-server) |
| 3 | Send a text request | ⏳ device; ✅ host live |
| 4 | Receive a correct response | ⏳ device; ✅ host live |
| 5 | Authorized GPIO tool | ⏳ device via LLM. Tool path verified with the mock HAL; GPIO init verified on hardware (boot log) |
| 6 | Read sensor data | ✅ hardware: `chip_temp` read by the sampler (`/devices` shows a live value) |
| 7 | Return sensor data to the model | ✅ host live (mock sensor) |
| 8 | Multi-step tool workflow | ✅ host live; ⏳ device |
| 9 | Scan BLE devices | ✅ hardware |
| 10 | Read a BLE characteristic | ⏳ needs an allowlisted BLE sensor (e.g. ESS 0x2A6E) |
| 11 | Recover after Wi-Fi drop | ⏳ needs Wi-Fi (`/wifi drop`) |
| 12 | Reject an unauthorized tool call | ✅ host (unit + live: autonomous actuation refused) |
| 13 | Recover from a malformed response | ✅ host (unit) |
| 14 | Deterministic automation without the LLM | ✅ host (unit); on hardware the rule runs in the `events` task with no network configured. A live trip needs chip temperature > 60 °C or a lower threshold in a test profile (checklist) |

Not verified at all:

- Anthropic against the real API (no key)
- Ollama, LM Studio, vLLM, OpenAI live
- BLE peripheral pairing from a phone
- OTA and rollback on hardware
- the secure-boot / flash-encryption build on hardware (irreversible; not done on this board)
- power measurement
