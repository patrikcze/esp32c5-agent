# LLM providers

| Provider | `kind` | Path | Auth | Tool protocol |
|---|---|---|---|---|
| OpenAI-compatible | `openai` | `{base}/chat/completions` | `Authorization: Bearer` (optional) | Chat Completions `tools` / `tool_calls` / role `tool` |
| Anthropic | `anthropic` | `{base}/v1/messages` | `x-api-key`, `anthropic-version: 2023-06-01` | Messages `tool_use` / `tool_result` |

Both support streaming (SSE) and non-streaming. `/provider stream on|off`.

## Adding a provider

1. Write `components/llm_provider/provider_x.c` exporting an `llm_provider_t`.
   It needs `build`, `headers`, `on_sse`, `on_body`, capabilities, and paths.
2. Add it to the `s_providers[]` table and the `llm_kind_t` enum in
   `llm_common.c` / `llm_provider.h`.

The agent core needs no change.

## Bounded parsing

- **Non-streaming:** the body accumulates in the response buffer (16 KB with
  PSRAM, 8 KB without). A larger body fails with `too_large` and is never
  executed.
- **Streaming:** only one SSE line is held at a time, in the same buffer.
  Each `data:` payload is tokenised into a fixed token array. Tool arguments
  accumulate in a 512-byte buffer per call; anything larger fails with
  `arguments_too_large`.
- **Completion rules.** OpenAI needs a `finish_reason`; `[DONE]` is
  recommended, and its absence is only a diagnostic. Anthropic needs
  `message_stop`, a `stop_reason`, and `content_block_stop` for every
  `tool_use` block.

If any rule fails, the response is truncated or invalid, and nothing from it
is executed.

## Server quirks handled (OpenAI-compatible)

| Quirk | Handling |
|---|---|
| server ignores `stream:true` and returns JSON | detected from the first byte |
| `finish_reason:"stop"` with complete tool calls (older llama.cpp/Ollama) | accepted; diagnostic `FINISH_MISMATCH` |
| tool call without `index` | positional |
| `arguments` sent as a JSON object instead of a string | copied as-is |
| tool call without `id` | rejected unless `/provider compat on` (then a local id is made; diagnostic) |
| `finish_reason:"tool_calls"` but no calls | protocol error. The server probably dropped them; it is reported, never guessed |
| `finish_reason:"length"` during a tool call | the call is never executed |
| model writes `<tool_call>…` / `[TOOL_CALLS]` as text | **not executed.** A warning explains that the server's tool parser did not recognise it (llama.cpp needs `--jinja` and a tool-capable template) |

Some backends discard tool calls before they reach the device, for example
when the parser for a chat template is missing. In that case the device can
only see the symptoms listed above. It cannot reconstruct the call, and it
deliberately does not try.

## Tested servers

| Server | Mode | Result | How |
|---|---|---|---|
| llama.cpp `llama-server` 0.8.0 (`--jinja`), Gemma 4 E4B Q4_K_M | stream + non-stream | multi-step workflow PASS; autonomous actuation refused | `live_agent` from the host (2026-11-01) |
| Anthropic API | — | not run (no key available) | fixtures + mock-transport tests |
| Ollama, LM Studio, vLLM, OpenAI | — | not run | fixtures cover their documented formats |

Run the live test yourself:

```bash
cmake -S tests/host -B build-host && cmake --build build-host
```
```bash
./build-host/live_agent openai http://127.0.0.1:8080/v1 <model> 1
```
```bash
LLA_API_KEY=sk-ant-... ./build-host/live_agent anthropic https://api.anthropic.com claude-haiku-5-5 1
```

`live_agent` uses libcurl for `https:` URLs (certificate verification on) and a minimal socket client for plain `http:`. On the device, TLS is provided by esp_http_client.

## Repeated tool descriptions

Both APIs are stateless, so tool schemas are sent with every request. The
cost is reduced in three ways:

- only tools permitted for the request's source are advertised
- schemas are compact (`additionalProperties:false`, short descriptions)
- for Anthropic, a `cache_control` breakpoint on the system block caches
  `tools + system` (prompt caching), which reduces cost and latency, not bytes on the wire
