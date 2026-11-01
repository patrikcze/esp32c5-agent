# Build, flash, configure

## Toolchain (pinned)

The project uses **ESP-IDF v5.5.6**. This is the first release line with
official ESP32-C5 support, and it is still maintained. It was verified to
build this project on 2026-11-01.

```bash
mkdir -p ~/esp && cd ~/esp
```
```bash
git clone -b v5.5.6 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git esp-idf-v5.5.6
```
```bash
cd ~/esp/esp-idf-v5.5.6 && ./install.sh esp32c5
```

On macOS with the python.org Python, the install can fail with
`CERTIFICATE_VERIFY_FAILED`. Point Python at a CA bundle and run it again:

```bash
SSL_CERT_FILE=/opt/homebrew/etc/ca-certificates/cert.pem ./install.sh esp32c5
```

**Arduino IDE is not supported.** The project relies on ESP-IDF features
(Kconfig, custom partitions, NimBLE configuration, secure boot) that the
Arduino IDE does not expose. VS Code with the Espressif ESP-IDF extension
works fine.

## Build variants

| Variant | Command |
|---|---|
| release (default) | `idf.py build` |
| debug | `idf.py -B build-debug -D SDKCONFIG=build-debug/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.debug" build` |
| hardened | see [SECURITY.md](SECURITY.md) |
| without BLE | add `CONFIG_LLA_ENABLE_BLE=n` (or `CONFIG_BT_ENABLED=n`) in a defaults overlay |

If you change `sdkconfig.defaults`, delete `sdkconfig` before building.

## Flash and monitor

```bash
. ~/esp/esp-idf-v5.5.6/export.sh
```
```bash
idf.py -p /dev/cu.usbserial-110 flash monitor
```

On the DevKitC-1, use the **UART** USB-C port: the console runs on UART0
(GPIO11/12). Exit the monitor with Ctrl+].

## Wi-Fi provisioning without a cable

If no Wi-Fi credentials are stored, the board starts provisioning by
itself. You can also start it with `/wifi prov`, or by holding the **BOOT**
button for 5 s.

1. The board opens an access point named `PROV_XXXXXX`.
2. Install Espressif's **ESP SoftAP Provisioning** app (iOS/Android), or use
   `$IDF_PATH/tools/esp_prov/esp_prov.py --transport softap --sec_ver 1 --pop <PoP>`.
3. Enter the proof-of-possession (PoP). It is a per-device 8-character code,
   shown by `/wifi prov info` and printed when provisioning starts. You can
   instead scan the QR payload printed there.
4. Pick your network and enter the password. The board joins the network
   and stores the credentials in the secret namespace.

Provisioning stops automatically after 10 minutes. `/wifi prov stop` ends it
early and returns to the previously stored network. Build with
`CONFIG_LLA_WIFI_PROV=n` to leave it out, which saves about 95 KB of flash.

## First-time configuration (serial console)

```
/wifi set <ssid> <password>
/provider use openai                          # or anthropic
/provider url http://<lan-ip>:8080/v1         # llama-server, Ollama: http://<ip>:11434/v1, LM Studio: :1234/v1
/provider model <model-id>                    # /models lists them
/provider key <api-key>                       # only for providers that need one
/provider stream on
/status
/ask What is the chip temperature?
```

## Notes for local servers

- **llama-server:** start it with `--jinja` (tool-call parsing) and
  `--host 0.0.0.0` so the board can reach it. Use a tool-capable model
  (Qwen 2.5/3, Llama 3.1+, Gemma 4, Mistral).
- **Ollama:** set `OLLAMA_HOST=0.0.0.0`; the base URL is `http://<ip>:11434/v1`.
- **LM Studio:** enable "Serve on local network"; the base URL is `http://<ip>:1234/v1`.
- **HTTPS with a private CA:** set `/provider url https://…`, then enter the
  CA one PEM line per command: `/provider ca begin`, then
  `/provider ca add <line>` for every line, then `/provider ca end`. The PEM
  is parsed on `end` and rejected if damaged. OTA uses the same CA.
- **Plain HTTP:** only accepted when the request carries no API key.

## OTA

```
/ota https://my-server/llmagent.bin
```

The new image runs in pending-verify state and is rolled back unless it
passes the health check (see SECURITY.md).

## Device profile

```
/devices profile          # show
/devices profile load     # paste one-line JSON (see examples/)
/devices profile reset    # back to built-in default after reboot
```
