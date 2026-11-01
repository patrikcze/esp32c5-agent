# llmagent — embedded AI agent for ESP32-C5

`llmagent` runs on an ESP32-C5. It sends requests to an LLM you choose
(llama.cpp, Ollama, LM Studio, vLLM, OpenAI, or Anthropic) and runs the tool
calls the model sends back. Every tool call is checked against a local policy
before it can touch hardware. The device also has deterministic local safety
rules that keep working when the network or the model is unavailable.

```
you> /ask Check the temperature and switch on the cooling fan if it is above 28 degrees
  [agent] REQUESTING_MODEL openai
  [tool] read_sensor {"device":"room_temp"} -> ok {"value":29.5,"unit":"C"}
  [tool] set_actuator {"device":"cooling_fan","value":1} -> ok {"value":1,"verified":true}
The temperature is 29.5 °C, above 28 °C; the cooling fan is now on.
```

## Status (what is verified, and how)

| Area | Status | Evidence |
|---|---|---|
| Firmware build (ESP-IDF v5.5.6, esp32c5) | ✅ | `idf.py build` → 1.69 MB image, 14 % OTA-slot headroom |
| Boot on ESP32-C5-DevKitC-1 (rev v1.0, 8 MB flash, 4 MB PSRAM) | ✅ hardware | serial log, `/memory` |
| Console, device registry, GPIO/RGB/temperature sensor init | ✅ hardware | `/devices` shows live chip temperature |
| BLE scan (central) | ✅ hardware | `/ble scan` found nearby advertisers |
| Agent loop + OpenAI-compatible protocol with a **real** server | ✅ host | `live_agent` vs llama-server + Gemma 4 E4B (stream and non-stream) |
| Agent state machine, both protocols, failure handling | ✅ host | 523 checks under ASan/UBSan |
| Wi-Fi, TLS, LLM tool workflow **from the device** (fan via GPIO readback) | ✅ hardware | 13 of 14 checklist items pass on the board, see docs/TESTING.md |
| Anthropic against the live API | ⏳ needs API key | fixtures + mock-transport tests only |
| BLE connect/read on a real sensor, peripheral pairing | ⏳ needs a BLE sensor / phone | checklist items 9–10 |

[docs/TESTING.md](docs/TESTING.md) lists every remaining verification gap.

## Quick start

```bash
. ~/esp/esp-idf-v5.5.6/export.sh
```
```bash
idf.py set-target esp32c5 build flash monitor -p /dev/cu.usbserial-110
```

Then, on the device console:

```
/wifi set MySSID MyPassword
/provider use openai
/provider url http://192.168.1.20:8080/v1      # llama-server --jinja, Ollama /v1, LM Studio...
/provider model qwen2.5-7b-instruct
/ask What is the chip temperature?
```

For Anthropic: `/provider use anthropic`, then `/provider key sk-ant-…`.
The key is stored in the secrets namespace and is never printed.

## Documentation

| | |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | layers, components, tasks, memory budget |
| [docs/AGENT-DESIGN.md](docs/AGENT-DESIGN.md) | state machine, budgets, idempotency, tool schema |
| [docs/PROVIDERS.md](docs/PROVIDERS.md) | OpenAI-compatible and Anthropic protocols, server quirks |
| [docs/HARDWARE.md](docs/HARDWARE.md) | pin table, device profiles, electrical requirements |
| [docs/BLUETOOTH.md](docs/BLUETOOTH.md) | central/peripheral, allowlist, pairing, coexistence |
| [docs/SECURITY.md](docs/SECURITY.md) | threat model, TLS, secrets, secure boot, flash encryption |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | measured numbers only |
| [docs/BUILD.md](docs/BUILD.md) | toolchain, build variants, flashing, OTA |
| [docs/TESTING.md](docs/TESTING.md) | host tests, live test, hardware checklist, gaps |
| [examples/](examples/) | ready-to-load device profiles |

## Not supported

- **Bluetooth Classic.** The ESP32-C5 has only Bluetooth LE.
- **Remote HTTP/WebSocket API.** Not implemented; the serial console and the
  BLE request characteristic are the only ways to send requests (see docs/SECURITY.md).
- **SPI devices.** There is no agent tool for SPI. Raw SPI access would be an
  unrestricted bus write, so it would need a device-specific driver first.
