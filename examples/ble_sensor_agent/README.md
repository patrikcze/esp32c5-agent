# BLE sensor agent

Reads a standard Environmental Sensing peripheral: Temperature 0x2A6E,
Humidity 0x2A6F, Battery 0x2A19. Examples are a Xiaomi thermometer running
custom ATC/pvvx firmware, or an nRF52 dev kit running the ESS sample.

1. `/devices profile load` with `profile.json`.
2. `/ble scan 5`, then find your sensor's address.
3. `/ble allow aa:bb:cc:dd:ee:ff` (read-only; add `rw` only if writes are needed).
4. `/ask Connect to the BLE thermometer, read its temperature and battery, then disconnect.`

Expected tool calls:

`ble_list_devices` → `ble_connect` → `ble_discover_services`
→ `ble_read_characteristic {"uuid":"2a6e"}` (decoded °C)
→ `ble_read_characteristic {"uuid":"2a19"}` → `ble_disconnect`

The model cannot connect to devices that are not on the allowlist, and it
cannot add devices to it.
