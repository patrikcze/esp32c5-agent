# Agent design

## State machine (`components/agent_core/agent.c`)

| From | Event | To |
|---|---|---|
| IDLE | job | CONNECTING |
| CONNECTING | network up (≤ `net_wait_ms`) | REQUESTING_MODEL |
| CONNECTING | no network | ERROR |
| REQUESTING_MODEL | body received | PARSING_RESPONSE |
| REQUESTING_MODEL / PARSING_RESPONSE | retryable failure, budget left | RECOVERING → REQUESTING_MODEL |
| PARSING_RESPONSE | text only | VERIFYING → COMPLETED |
| PARSING_RESPONSE | complete tool calls | VALIDATING_TOOL |
| VALIDATING_TOOL | accepted | EXECUTING_TOOL [→ VERIFYING if a postcondition exists] |
| VALIDATING_TOOL | rejected | next call (rejection becomes the tool result) |
| last call done | — | SUBMITTING_RESULT → REQUESTING_MODEL |
| any | cancel flag | CANCELED |
| any | fatal / budget | ERROR |

The brief listed the order SUBMITTING_RESULT → VERIFYING. This
implementation verifies *each* side effect right after it executes (readback),
so the verified result reaches the model. The final VERIFYING step before
COMPLETED reports unknown outcomes from the run.

On ERROR or CANCELED, the run's messages are removed from the context, so
later runs never see a dangling `tool_use` without its result.

## Budgets (defaults; `/agent budget <turns> <calls> <secs>`)

| Budget | Default |
|---|---|
| model turns per run | 8 |
| executed tool calls per run | 16 |
| identical (tool, args) executions | 2 |
| repeated-call strikes before abort | 2 |
| retries per model request | 3 (500 ms × 2ⁿ, ≤ 8 s, +jitter, honours Retry-After ≤ 60 s) |
| wall-clock deadline | 120 s |

## Retry classification

| Failure | Retried? | Why |
|---|---|---|
| connect / timeout / connection dropped / no network | yes | model requests have no side effects |
| HTTP 408, 409, 429, 500, 502, 503, 504, 529 | yes | transient |
| in-stream `error` event (e.g. `overloaded_error`) | yes | transient |
| truncated, malformed or protocol-violating response | yes | nothing was executed (I1) |
| TLS verification failure, bad URL, plain HTTP with a key | no | configuration |
| HTTP 400, 401, 403, 404 | no | the request itself is wrong |

## Idempotency and the "lost response" case

Every tool declares one of three effect classes: `NONE`, `IDEMPOTENT`
(setting a state again is harmless), or `NON_IDEMPOTENT` (a pulse, bus
write, UART send, or BLE write). The agent keeps a per-run ledger of
`(hash(tool, normalised args), last status, count)`:

- If a model request fails *after* tools ran, the retry re-sends the same
  context, which already contains the results. Tools never run again because
  of a transport retry.
- If the model asks for the same non-idempotent call again with identical
  arguments, the call is rejected with `duplicate_suppressed`.
- If an execution's outcome is uncertain (`UNKNOWN`: a bus write timed out, or
  a readback disagrees), that call is never repeated automatically. Other
  calls to the same non-idempotent tool are also blocked for the rest of the
  run, and the final result tells the user to check the device.

All of this is tested in `tests/host/test_agent.c`:
`test_lost_response_no_replay` and `test_unknown_outcome_not_retried`.

## Tool definition

```c
{.name = "set_actuator", .desc = "...", .args = A_ACT, .n_args = 3,
 .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT,
 .timeout_ms = 200, .exec = x_set_actuator, .verify = v_set_actuator}
```

The argument schema is a compact subset: `integer|number` with an inclusive
range, `boolean`, `string` with `maxLength`, and string `enum`. Schemas
always set `additionalProperties:false`. Unknown or duplicate keys, wrong
types, out-of-range values, and missing required arguments are all rejected
before `exec` runs. Timeouts are cooperative: each tool receives an absolute
deadline and must bound its blocking calls (I2C, UART and BLE timeouts are
derived from it).

## Bounded tool results

A tool result holds at most 768 bytes, and every list tool stays within that
limit. Device, BLE and GATT lists add entries until the next one would not
fit, then report `shown`/`total` (or `devices_omitted`), so the model learns
that the list is partial. Earlier versions replaced an oversized result with
`result_truncated` and lost everything.

## Permissions

The permission levels are READ < ACTUATE < CONFIG < DANGEROUS, with one
ceiling per request source:

| Source | Default ceiling |
|---|---|
| console | CONFIG |
| autonomous (event triggers) | READ |
| ble (request characteristic) | READ |
| remote | READ |

Tools above a source's ceiling are not even advertised to the model.
`/policy` can raise ceilings, deny individual tools, or allow specific
ACTUATE tools for the autonomous source. A device must also have
`llm_write: true` in the *local* profile before the agent can actuate it.

## Context

The context is an arena of compact records (`llm_conv.c`, 5-byte header).
When space is needed, whole older runs are evicted, never single messages. A
single run that exceeds the arena fails with "context budget exhausted".
`/agent clear` or `/reset context` clears it. Nothing is written to flash.

## Autonomy

- **Deterministic rules** run in the `events` task on every sensor sample
  (see HARDWARE.md). They do not use the LLM.
- **Triggers** map an event (for example a `gpio_edge` on `boot_button`) to
  an agent prompt. Each trigger has a minimum interval of at least 5 s. It is
  skipped while the agent is busy, and the run uses the `autonomous` source
  (READ by default).
- **Rule proposals.** The model can call `propose_rule`. This only stores a
  pending proposal; a person must run `/rules approve` on the console.
  Approved rules never receive safety locks.
