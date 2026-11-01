# Testing

## 1. Host tests (no hardware)

```bash
cmake -S tests/host -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure
```

The suite runs 539 checks across 10 test groups, compiled with
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

## Verification status and gaps (updated 2026-11-01, on-device run)

Setup: DevKitC-1 on Wi-Fi "SweetHome" (2.4 GHz, ch 3). llama-server 0.8.0
(`--jinja`, Gemma 4 E4B Q4_K_M) ran on a Mac on the same LAN over plain
HTTP, with no API key.

| # | Requirement | Status | Observed on hardware |
|---|---|---|---|
| 1 | Connect to Wi-Fi | ✅ | IP 192.168.88.36, RSSI −63…−72; SNTP synced |
| 2 | Authenticate to an LLM | ✅ | `/models` returned the GGUF id. Anthropic over TLS: cert-bundle validation OK, 401 for the missing key parsed and not retried |
| 3 | Send a text request | ✅ | "Say hello…" COMPLETED in 6.3 s |
| 4 | Receive a correct response | ✅ | |
| 5 | Authorized GPIO tool | ✅ | `set_actuator cooling_fan 1`: GPIO23 readback 1, `verified:true` |
| 6 | Read sensor data | ✅ | `read_sensor chip_temp` → 32.6 °C |
| 7 | Return sensor data to the model | ✅ | the model's answer quoted 32.6 °C |
| 8 | Multi-step tool workflow | ✅ | capabilities → read → decide → actuate → answer; 5 turns, 23 s |
| 9 | Scan BLE devices | ✅ | 16 devices in a 3–4 s scan |
| 10 | Read a BLE characteristic | ⏳ | needs an allowlisted ESS sensor |
| 11 | Recover after a Wi-Fi drop | ✅ | `/wifi drop` → reconnected in 2495 ms; the next request OK |
| 12 | Reject an unauthorized call | ✅ | `/policy console read`: the actuator tool is not offered and the fan is untouched (also a host unit test) |
| 13 | Recover from a malformed response | ✅ | test server sent a truncated `set_actuator` call; 4 attempts rejected, ERROR, fan unchanged |
| 14 | Deterministic automation while the LLM is unavailable | ✅ | provider unreachable (port 9): rule `chip_temp > 20` switched the fan on and set LOCKED; the agent's retries failed gracefully |

Additional hardware results (second session):

| Check | Observed |
|---|---|
| Safety lock vs. the agent | rule `chip_temp > 20` held the fan; agent `set_actuator 0` → `locked_by_safety_rule`; fan stayed on, and the model explained why |
| Event → autonomous run | `rule_on` trigger started an autonomous run that read and reported the temperature |
| OTA from a LAN HTTPS server with a private CA | 1.69 MB written in 12.9 s, rebooted into the other slot, health check passed, marked valid |
| OTA rollback | image with a forced health-check failure → `rolling back` → bootloader returned to the previous image |
| BLE scan impact on LLM latency | short request 362 ms average without scanning vs. 380 ms with continuous scanning (+5 %) |
| Streamed token usage | reported (e.g. 2371 in / 603 out) |

Still open:

- **BLE:** characteristic read on a real sensor (item 10) and peripheral pairing from a phone.
- **Other servers and providers:** Anthropic with a real key (user-run; see below); Ollama, LM Studio, vLLM, OpenAI.
- **Secure build** on hardware: irreversible, so not done.
- **Power measurement.**
