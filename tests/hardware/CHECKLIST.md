# Hardware test checklist (ESP32-C5-DevKitC-1)

Fill in this table for every firmware release. Only results observed on real
hardware count; results from mocks or the host do not.

**Setup**

- Flash the firmware: `idf.py -p <port> flash monitor`.
- Have an OpenAI-compatible server on the LAN (for example
  `llama-server --jinja --host 0.0.0.0`).
- Optional: a BLE sensor exposing Environmental Sensing (0x181A / 0x2A6E).
- Optional: an LED with a 330 Ω resistor from GPIO23 to GND, standing in for `cooling_fan`.

| # | Step | Expected | Result / date / commit |
|---|---|---|---|
| 0 | Reset; read boot log | `flash 8 MB physical`, PSRAM line, `1 rules, 1 triggers loaded`, `BLE ready`, no `E (` lines | |
| 1 | `/wifi set <ssid> <pass>`; `/status` | `connected`, IP, `time=synced` within ~10 s | |
| 2 | `/provider url …`, `/provider model …`; `/models` | model list printed (HTTP 200) | |
| 3 | `/ask Say hello in five words.` | `COMPLETED`; reply printed; `/memory` agent stack HWM > 2 KB | |
| 4 | inspect reply | sensible text, `0 tool calls` | |
| 5 | `/ask Turn on the cooling fan.` | `[tool] set_actuator … verified:true`; GPIO23 high (LED on); `/devices` shows 1 | |
| 6 | `/devices read chip_temp` | plausible die temperature (20–60 °C) | |
| 7 | `/ask What is the chip temperature?` | `read_sensor chip_temp` executed; answer matches item 6 ±1 °C | |
| 8 | `/ask Read the chip temperature and switch the cooling fan on if it is above 20 C, otherwise off.` | 2+ tool calls, correct branch, `COMPLETED` | |
| 9 | `/ble scan 5` | devices with RSSI listed; scan returns in ~5 s | |
| 10 | `/ble allow <addr>`; `/ble connect <n>`; `/ble services`; `/ble read 2a6e` | value decoded in °C | |
| 11 | `/wifi drop`; wait; `/wifi` | reconnects automatically; `last_reconnect` printed; next `/ask` works | |
| 12 | `/ask Write 0x55 to register 9 of expander` (with an `i2c` device whose `write_regs` excludes 9) **or** `/policy console read` then `/ask Turn on the fan` | `rejected … not_authorized` / `permission_denied`; no output change | |
| 13 | point `/provider url` at a server returning garbage (e.g. `python3 -m http.server`) and `/ask hi` | `model response malformed` retries, then `ERROR`; device keeps running | |
| 14 | load a profile with rule `chip_temp > 20` → `cooling_fan` (`examples/autonomous_controller`), unplug network or set a bad provider URL | fan switches on within one sample period; `/ask Turn off the fan` → `locked_by_safety_rule` | |
| 15 | press BOOT | `event gpio_edge boot_button` and an autonomous agent run (if a provider is configured) | |
| 16 | phone (nRF Connect): connect to `llmagent-XXXX`, write to `…03` | passkey shown on console; write works only after pairing | |
| 17 | `/bench llm 5`; `/wifi` | record `last_connect` (TLS cost) and reuse count in PERFORMANCE.md | |
| 18 | `/reset reboot` while the fan is on | fan goes off before restart (shutdown handler), boots with the fan off | |
