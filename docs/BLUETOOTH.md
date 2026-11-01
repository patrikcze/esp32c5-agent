# Bluetooth LE

The ESP32-C5 supports Bluetooth LE 5 only. Bluetooth Classic is **not**
available and is not implemented. The host stack is NimBLE (ESP-IDF v5.5.6),
with up to 2 connections and up to 4 bonds stored in NVS.

## Central

| Operation | Tool | Console | Limits |
|---|---|---|---|
| scan | `ble_scan` | `/ble scan [secs] [prefix]` | 0.5–10 s; table of 16 devices; name-prefix filter |
| list | `ble_list_devices` | `/ble list` | strongest signal first, as many as fit in one tool result; `shown`/`total` mark a partial list |
| connect | `ble_connect` | `/ble connect <n>` | **allowlisted addresses only**; one central link |
| discover | `ble_discover_services` | `/ble services` | 12 services / 32 characteristics |
| read | `ble_read_characteristic` | `/ble read <uuid>` | 64 bytes |
| write | `ble_write_characteristic` | — (agent only) | ≤ 20 bytes; only devices allowlisted with `rw` |
| notify | `ble_subscribe_notifications` | `/ble sub <uuid>` | finds and writes the CCCD; notifications become `ble_seen` events |

**Auto-reconnect.** After a connection you made with `ble_connect` or
`/ble connect` drops unexpectedly, the board reconnects in the background.

- **Backoff:** 1 s, doubling to 30 s, at most 6 attempts.
- **On every attempt** the allowlist is checked again. The GATT table is kept,
  and notification subscriptions are re-enabled.
- **It stops** when you disconnect deliberately or when the attempts run out.
- **Status:** `/ble` shows whether reconnect is armed or pending.
- **Not yet hardware-verified:** this needs a connectable allowlisted
  peripheral.

The allowlist is managed locally (`/ble allow aa:bb:cc:dd:ee:ff [rw]`,
`/ble deny …`) and persisted. The model can never add devices to it. Some
phones and sensors use resolvable private addresses that change over time.
Such devices must be bonded or use a static address to stay allowlisted.

**Device profiles.** Decoding is implemented only for standard GATT
characteristics:

| UUID | Characteristic | Format |
|---|---|---|
| 0x2A6E | Temperature | sint16, 0.01 °C |
| 0x2A6F | Humidity | uint16, 0.01 % |
| 0x2A19 | Battery Level | uint8 % |
| 0x2A6D | Pressure | uint32, 0.1 Pa |

Unknown characteristics are returned as hex. There is no generic "control
any BLE device" feature. Writes to proprietary GATT protocols need a device
profile, and are otherwise just raw writes to an allowlisted `rw` device.

## Peripheral

- **Advertised name:** `llmagent-XXXX`.
- **Service UUID:** `6c6c6167-656e-7400-0000-000000000000`.

| Characteristic (last byte) | Properties | Security | Content |
|---|---|---|---|
| `…01` status | read | none | uptime, Wi-Fi state, agent busy |
| `…02` capabilities | read | encrypted | device list |
| `…03` request | write | encrypted + MITM (passkey) | text queued as an agent request with source `ble` |

Pairing uses LE Secure Connections with passkey display: the device prints a
6-digit key on the serial console, and the phone enters it. A `ble`-source
request has the READ ceiling by default, so a paired phone can ask questions
but cannot actuate anything unless `/policy ble actuate` is set.

## Coexistence

The 2.4 GHz radio is shared between Wi-Fi and BLE. The firmware sets:

- `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`
- Wi-Fi modem sleep (`WIFI_PS_MIN_MODEM`), which is mandatory with coexistence
- bounded scans with interval 50 ms and window 30 ms (60 % duty)

**Measured:** a 3–4 s scan returns on time (3002 ms wall time for a 3000 ms
scan) and finds 16 devices.

**Not yet measured:** HTTP latency during a BLE scan. Use `/bench llm 5`
with and without a parallel `/ble scan 10` once Wi-Fi is configured
(checklist item 9).
