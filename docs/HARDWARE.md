# Hardware

## Target

- **SoC:** ESP32-C5 (RISC-V, single core, 240 MHz). It has dual-band Wi-Fi 6,
  Bluetooth LE 5 (**no Bluetooth Classic**), and 802.15.4 (unused).
- **Board:** ESP32-C5-DevKitC-1. The unit used for testing reported chip
  rev v1.0, 8 MB flash (the image is laid out for 4 MB) and 4 MB PSRAM
  (detected at boot).
- **Flash and PSRAM are detected at runtime** (`/status`, the
  `system_info` tool). `CONFIG_SPIRAM_IGNORE_NOTFOUND=y`, so boards without
  PSRAM still boot and use smaller buffers.

## Pin table (`components/peripheral_registry/board_esp32c5.c`)

| GPIO | Class | Notes |
|---|---|---|
| 0, 1 | caution | XTAL_32K (usable if no 32 kHz crystal); 1 = ADC1_CH0 |
| 2, 3 | strapping | MTMS/MTDI, ADC1_CH1/CH2 |
| 4, 5, 6 | free | ADC1_CH3..CH5 |
| 7 | strapping | |
| 8, 9, 10, 23, 24 | free | general purpose |
| 11, 12 | **reserved** | UART0 console |
| 13, 14 | **reserved** | USB-Serial-JTAG |
| 15 | **reserved** | PSRAM CS on modules with PSRAM |
| 16–22 | **reserved** | SPI flash / PSRAM |
| 25, 26 | strapping | |
| 27 | strapping | on-board WS2812 RGB LED |
| 28 | strapping | BOOT button |

Sources: ESP-IDF v5.5.6 `soc/esp32c5` headers, the ESP-IDF GPIO docs, and
the DevKitC-1 user guide. **Verify against your board revision.** A pin
that exists on the SoC is not necessarily safe to wire on a given board.

Validation rules:

- Reserved pins can never be assigned.
- Strapping pins need `"allow_strapping": true`. The exceptions are the
  on-board RGB LED and BOOT-button inputs.
- ADC devices need an ADC-capable pin.
- Each pin can belong to only one device. The I2C bus pins are shared by I2C devices.
- Only UART1 is offered; UART0 is the console.

## Device profile

The profile is JSON stored in NVS (`/devices profile load`). If none is
stored, the built-in `main/default_profile.json` is used. Profiles are
validated completely before they are applied. When a profile is rejected,
nothing changes.

| type | keys |
|---|---|
| `gpio_out` | `pin`, `active_low`, `safe` (0/1), `llm_write`, `max_on_s`, `min_toggle_ms`, `open_drain` |
| `gpio_in` | `pin`, `active_low`, `pull` (`up`/`down`/`none`), `debounce_ms`, `poll_ms` |
| `adc` | `pin`, `kind` `linear` (`a`,`b`: value = mV·a + b, `unit`) or `ntc` (`beta`,`r0`,`rs`,`vref_mv`; NTC on the low side) |
| `pwm` | `pin`, `freq` (1–40000 Hz), `min`, `max`, `safe` (%), `llm_write` |
| `rgb` | `pin` (WS2812 via RMT), `max` brightness %, `llm_write` |
| `chip_temp` | internal die temperature sensor (not ambient) |
| `sht3x` | `sda`, `scl`, `addr` (0x44/0x45), `hz` — temperature + humidity with CRC check |
| `i2c` | `sda`, `scl`, `addr`, `readable`, `write_regs` (allowlist of ≤ 8 registers) |
| `uart` | `tx`, `rx`, `baud`, `max_len` (≤ 128), `llm_write` |

Top level keys:

- `board`
- `allow_strapping`
- `devices`
- `spare_pins` (pins the agent may configure through `gpio_configure`)
- `rules`
- `triggers`

See [examples/](../examples).

## Safety behaviour (independent of the LLM)

- **Safe state at boot.** The profile is applied before Wi-Fi, BLE, or the
  agent start. Output levels are latched *before* the output driver is
  enabled, so pins do not glitch.
- **Safe state on restart.** A shutdown handler (`esp_register_shutdown_handler`)
  drives every output to its safe state before a software restart, OTA, or
  factory reset.
- **Max on-time watchdog.** `max_on_s` forces the safe state, unless a
  safety rule holds the device.
- **Toggle rate limit.** `min_toggle_ms` limits how fast relays can be
  switched by the agent and the console.
- **Safety rules lock the actuator.** The agent and the console get
  `locked_by_safety_rule` until the rule releases.
- **Stale sensor.** With `stale_s`, a rule whose sensor stops reporting
  applies its `fault` value. For cooling, that means the fan turns on.
- **Bus writes.** I2C writes are limited to allowlisted registers. A failed
  write is reported as *uncertain* and is never retried automatically.

## Electrical requirements for actuators

An ESP32-C5 GPIO is a 3.3 V logic signal with limited current. It must never
power a load directly.

- **Relays:** use a relay module with a driver transistor and a flyback diode,
  or an SSR rated for the load. Mains wiring must be done by a qualified person.
- **Fans and motors:** use a logic-level N-MOSFET (low-side) with a flyback
  diode across the motor and a gate resistor plus pull-down, so the fan is
  OFF while the MCU is in reset. Set `safe` accordingly.
- **PWM fans:** 4-pin fans take a 25 kHz open-drain PWM input. Check the fan's datasheet.
- **Strapping pins:** do not drive a load from a strapping pin unless you
  understand its boot-time level (`allow_strapping` exists for that case).
- **Common ground:** all supplies must share ground. Protect the ESP32's
  pins from back-feeding when it is unpowered.
- **Heaters and other high-energy loads:** use an independent hardware
  thermal cut-off. Software rules are not a substitute.
