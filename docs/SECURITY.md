# Security and safety

## Threat model

- **The LLM is untrusted input.** That includes its tool names, arguments,
  and text. A compromised or confused model, a prompt injection through
  sensor names or BLE device names, and a malicious provider endpoint must
  not be able to cause unsafe physical actions.
- **The local operator at the serial console is trusted.** Anyone with
  physical access to the UART can reconfigure the device; protect physical
  access accordingly.
- **The network is hostile.** TLS is always verified.

## Controls and where they are enforced

| Control | File |
|---|---|
| tool allowlist (registry), argument schema validation, unknown keys rejected | `tool_registry/tool_registry.c` |
| per-source permission ceilings, deny list, side-effect rate limit | `tool_registry/tool_registry.c` |
| device-level authorisation (`llm_write`, I2C register allowlist, UART `llm_write`) | `peripheral_registry/periph.c` |
| pin reservation, strapping protection, ownership | `peripheral_registry/periph.c`, `board_esp32c5.c` |
| safe states (boot, shutdown, fault), max on-time watchdog, rule locks | `periph.c`, `event_engine/` |
| never execute partial/truncated/prose tool calls | `llm_provider/llm_common.c` |
| no automatic replay of uncertain side effects | `agent_core/agent.c` |
| bounded execution (turns, calls, deadline, retries, loop detection) | `agent_core/agent.c` |
| TLS verification (x509 bundle or private CA), no skip option | `network_manager/http_transport_esp.c` |
| no credentials over plain HTTP; plain HTTP can be compiled out | same, `CONFIG_LLA_ALLOW_PLAIN_HTTP` |
| secrets in a separate NVS namespace, never exported or printed | `config_manager/`, `/config` |
| header copy holding the API key wiped after each request | `agent.c` (`hdr_scratch`) |
| BLE: allowlist for connect, `rw` flag for writes, encrypted + MITM for peripheral writes | `ble_manager/` |
| persistent rules only after local `/rules approve` | `event_engine/event_task_esp.c` |
| task watchdog on the control loop | `event_task_esp.c` |
| signed OTA + rollback | `ota_esp.c`, `app_main.c` |

Logs never contain API keys or the Wi-Fi password. Configuration output
shows only `<set, N chars>`. ESP-IDF's HTTP client logs headers only at
verbose level, which is compiled out (`CONFIG_LOG_MAXIMUM_LEVEL_INFO`).

## Credentials

- Credentials are entered on the console (`/wifi set`, `/provider key`) and
  stored in NVS namespace `llasec`.
- The Wi-Fi driver is told to keep its own copy in RAM only
  (`WIFI_STORAGE_RAM`).
- **Nothing secret is in the repository.** `.gitignore` excludes `*.pem`,
  signing keys, and `sdkconfig`.

**Weakness without NVS encryption.** Anyone with physical access can read
NVS by dumping the flash. For production, enable flash encryption together
with NVS encryption (below).

## Production hardening (`sdkconfig.secure`)

> ⚠️ This burns eFuses. The changes are **irreversible**. Practise on a
> spare board first.

1. **Secure Boot V2 (RSA-3072 or ECDSA).** Generate the signing key once
   and keep it offline:
   ```bash
   espsecure.py generate_signing_key --version 2 secure_boot_signing_key.pem
   ```
   The app image and OTA images are verified before boot.
2. **Flash encryption, release mode.** Also enables encrypted NVS through the
   `nvs_keys` partition (already in `partitions.csv`, flagged `encrypted`).
3. **UART ROM download mode** is restricted to secure download mode.
4. **Plain HTTP is compiled out** (`CONFIG_LLA_ALLOW_PLAIN_HTTP=n`). Use
   HTTPS with `/provider ca` for LAN servers that use a private CA.
5. **ESP32-C5 rev v1.0 erratum.** PSRAM contents are **not encrypted** even
   with flash encryption enabled (the boot log warns about this).
   `sdkconfig.secure` therefore keeps mbedTLS and network buffers in
   internal RAM:
   - `CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=y`
   - `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n`

   Agent request and response buffers stay in PSRAM. They contain prompts
   and tool data but no keys. If prompts are sensitive, build without PSRAM
   usage. Not yet measured: whether TLS + BLE fit in internal RAM under the
   secure overlay. Run `/memory` after building it.

Build the hardened image:

```bash
idf.py -B build-secure -D SDKCONFIG=build-secure/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secure" build
```

## OTA

- OTA is HTTPS only, with certificate verification (`/ota https://…`).
  Outputs go to their safe state first.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set. A new image is marked
  valid only after the health check: Wi-Fi associates within 60 s, or no
  credentials are configured. Otherwise the bootloader rolls back.
- With Secure Boot V2, unsigned images are rejected.

## Remote interface

No HTTP or WebSocket API is implemented. The brief marked it optional, and
an authenticated HTTPS server would cost about 40 KB of RAM for TLS server
sessions. The only remote entry point is the BLE request characteristic,
which requires an encrypted, MITM-authenticated, bonded link and is limited
to READ by default.
