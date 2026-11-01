# Performance

Every number here was measured. Rows marked "not measured" list the command
that will produce the figure.

Measurement setup: ESP32-C5-DevKitC-1, chip rev v1.0, 8 MB flash, 4 MB PSRAM,
ESP-IDF v5.5.6, default `sdkconfig.defaults` (`-Os`, log level INFO),
2026-11-01.

## Image and static memory (`idf.py size`)

| Metric | Value |
|---|---|
| app image | 1,692,362 B (OTA slot 1,966,080 B → 14 % free) |
| flash `.text` + `.rodata` | 1,543,718 B |
| HP SRAM used (static, incl. IRAM code) | 199,540 B of 320,928 (62 %) |
| `.bss` / `.data` | 50,632 B / 21,040 B |
| app image without BLE (`CONFIG_BT_ENABLED=n`) | 1,280,720 B (35 % slot free) |
| llmagent's own components (flash, approx.) | ~145 KB; the rest is Wi-Fi, BLE controller, mbedTLS, lwIP |

Before buffers were moved to on-demand and PSRAM allocation, `.bss` was
89,672 B and HP SRAM use was 238,358 B.

## Heap (serial log and `/memory`)

| Point | Internal free | Largest block | Min ever |
|---|---|---|---|
| after profile and tool registration (pre-Wi-Fi) | 177,595 B | 106,496 B | — |
| after Wi-Fi + BLE + agent start, idle | 88,959 B | 47,104 B | 33,620 B |
| same, first build (before PSRAM placement) | 38,691 B | 15,360 B | 18,940 B |
| PSRAM free, idle | 4,102,500 B | 4,063,232 B | — |

Not measured yet: heap during a TLS session. Run `/memory` right after an
`/ask` that used HTTPS.

## Task stacks (`/memory`, free bytes at the high-water mark, idle and after BLE scans)

| Task | Size | Free at HWM |
|---|---|---|
| events | 4096 | 1,988 |
| console_repl | 6144 | 3,908 |
| agent | 12288 | 11,808 (no TLS run yet; re-measure) |
| nimble_host | IDF | 1,960 |

## Parser

| Benchmark | Result |
|---|---|
| `/bench json`, 253-byte tool-call response, 31 tokens | 31.6 µs per parse (8.0 MB/s); token array 310 B; zero heap |

## Agent latency

**Measured from the host (Mac, llama-server, Gemma 4 E4B Q4_K_M):** the
3-turn temperature-and-fan workflow took 9.6 s streaming and 14.6 s
non-streaming. Model inference dominates, so these numbers describe the
model and the Mac, not the ESP32.

**Measured on the device (LAN llama-server, Gemma 4 E4B on a Mac):**

| Metric | Value |
|---|---|
| text-only request, end to end | 6.3 s (model generation dominates) |
| 5-turn tool workflow (3 tool calls) | 23.0 s total; tool execution 2 ms total |
| largest request body | 10,496 B (system prompt + 27 tool schemas + history) |
| TLS connection to api.anthropic.com (cert bundle) | 606 ms |
| new plain-HTTP connection on the LAN | 75–105 ms |
| Wi-Fi reconnect after `/wifi drop` | 2,495 ms (1 s backoff + association + DHCP) |
| internal heap minimum during agent runs | 26,200 B |
| agent task stack | 8,752–8,896 B free of 12,288 (worst case ~3.5 KB used) |
| `sys_evt` stack | 788 B free at 2,304 → raised to 3,584 (2,068 B free) |

llama-server closes idle keep-alive connections between turns. The
transport detects the dead socket before any response byte arrives and
reconnects once (~75 ms); no agent-level retry is needed.

| short LLM round trip (`/bench llm 5`, "OK" reply) | 362 ms avg (306–392) |
| same, BLE scanning continuously (`/bench llm 5 ble`) | 380 ms avg (358–399), +5 % |
| internal heap minimum with Wi-Fi + HTTP + continuous BLE scan | 18,840 B |
| OTA download + write (1.69 MB, HTTPS, private CA) | 12.9 s |
| Anthropic `claude-haiku-5-5`, 3-turn tool workflow, from the board over TLS | 3.1–3.5 s |

**Not measured on the device yet:**

| Metric | Command |
|---|---|
| power consumption | requires an ammeter on the J5 jumper (see the DevKitC-1 user guide) |

## BLE

| Metric | Value |
|---|---|
| scan 3000 ms | returned in 3002 ms wall time; 16 devices (table limit) |
