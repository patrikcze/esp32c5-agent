/*
 * llm_provider.h — provider-neutral LLM interface.
 *
 * Contents:
 *   - llm_conv_t      bounded conversation arena (compact records)
 *   - llm_response_t  parsed model turn (text + complete tool calls)
 *   - llm_transport_t HTTP transport abstraction (ESP impl in network_manager,
 *                     mock in tests)
 *   - llm_provider_t  per-protocol request builder + incremental parser
 *
 * Portable: depends on lla_common and tool_registry only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "lla_json.h"
#include "tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLM_MAX_CALLS 4     /* tool calls accepted per model turn */
#define LLM_ID_MAX 64
#define LLM_ARGS_MAX 512    /* raw JSON argument object per call */
#define LLM_TEXT_MAX 1024   /* assistant text kept per turn */

/* ------------------------------------------------------------ strings */

typedef struct {
    const char *p;
    uint16_t n;
} lla_str_t;

/* ------------------------------------------------------- conversation */

typedef enum { LLM_MSG_USER = 1, LLM_MSG_ASSISTANT = 2, LLM_MSG_TOOL_RESULTS = 3 } llm_msg_kind_t;

typedef struct {
    lla_str_t id, name, args;
} llm_call_view_t;

typedef struct {
    lla_str_t id, name, content;
    bool is_error;
} llm_result_view_t;

typedef struct {
    llm_msg_kind_t kind;
    uint16_t run_id;
    lla_str_t text;
    uint8_t n;
    llm_call_view_t calls[LLM_MAX_CALLS];
    llm_result_view_t results[LLM_MAX_CALLS];
} llm_msg_view_t;

typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t len;
    uint16_t evictions; /* statistics */
} llm_conv_t;

void llm_conv_init(llm_conv_t *c, uint8_t *buf, uint16_t cap);
void llm_conv_clear(llm_conv_t *c);
/** Append; evicts whole older runs (run_id != @p run_id) if needed. */
bool llm_conv_add_user(llm_conv_t *c, uint16_t run_id, const char *text, size_t n);
bool llm_conv_add_assistant(llm_conv_t *c, uint16_t run_id, const char *text, size_t n, const llm_call_view_t *calls,
                            uint8_t ncalls);
bool llm_conv_add_results(llm_conv_t *c, uint16_t run_id, const llm_result_view_t *res, uint8_t n);
/** Evict the oldest run unless it is @p keep_run. Returns false if nothing evictable. */
bool llm_conv_evict_oldest(llm_conv_t *c, uint16_t keep_run);
/** Remove every record of @p run_id (used to roll back a failed run). */
void llm_conv_drop_run(llm_conv_t *c, uint16_t run_id);
/** Iterate: *pos starts at 0. Returns false at end. */
bool llm_conv_next(const llm_conv_t *c, uint16_t *pos, llm_msg_view_t *out);
uint16_t llm_conv_count(const llm_conv_t *c);

/* ------------------------------------------------------------ response */

typedef enum {
    LLM_STOP_NONE = 0,     /* not seen (truncated) */
    LLM_STOP_END,          /* natural end of turn */
    LLM_STOP_TOOL_USE,     /* model requests tool execution */
    LLM_STOP_MAX_TOKENS,   /* output limit hit: anything partial is discarded */
    LLM_STOP_FILTER,       /* content filter / refusal */
    LLM_STOP_OTHER,
} llm_stop_t;

typedef struct {
    char id[LLM_ID_MAX];
    char name[TOOL_NAME_MAX];
    char args[LLM_ARGS_MAX];
    uint16_t args_len;
    bool started;
    bool complete;   /* stream: block closed / non-stream: always */
    bool overflow;   /* arguments exceeded LLM_ARGS_MAX */
    const char *invalid; /* non-NULL: complete but unusable call (reason); never executed */
    bool synth_id;   /* id synthesised under allow_missing_tool_ids */
} llm_tool_call_t;

/** Diagnostic flags collected while parsing (never change execution decisions). */
enum {
    LLM_DIAG_FINISH_MISMATCH = 1u << 0, /* tool calls with finish_reason "stop" */
    LLM_DIAG_SYNTH_ID = 1u << 1,
    LLM_DIAG_UNKNOWN_EVENT = 1u << 2,
    LLM_DIAG_TEXT_TRUNC = 1u << 3,
    LLM_DIAG_TOOLCALL_IN_TEXT = 1u << 4, /* prose resembles a tool call — NOT executed */
    LLM_DIAG_EXTRA_CALLS = 1u << 5,      /* more than LLM_MAX_CALLS requested */
    LLM_DIAG_NO_DONE = 1u << 6,          /* stream closed after finish without [DONE] */
};

typedef struct {
    llm_stop_t stop;
    char stop_raw[24];
    char text[LLM_TEXT_MAX];
    uint16_t text_len;
    llm_tool_call_t calls[LLM_MAX_CALLS];
    uint8_t n_calls;
    uint32_t in_tokens, out_tokens;
    uint32_t diag;
    char err_type[32];
    char err_msg[128];
} llm_response_t;

typedef enum {
    LLM_OK = 0,
    LLM_ERR_TRUNCATED = -1,  /* response ended before a terminal marker */
    LLM_ERR_MALFORMED = -2,  /* invalid JSON / SSE */
    LLM_ERR_TOO_LARGE = -3,  /* exceeded bounded buffers */
    LLM_ERR_PROTOCOL = -4,   /* well-formed JSON but missing/contradictory fields */
    LLM_ERR_API = -5,        /* provider returned an error object */
} llm_perr_t;

const char *llm_perr_str(int e);
const char *llm_stop_str(llm_stop_t s);

/* ------------------------------------------------------------ transport */

typedef struct {
    const char *name;
    const char *value;
} llm_header_t;

#define LLM_MAX_HEADERS 6

typedef struct {
    const char *method; /* "POST" or "GET" */
    const char *url;
    llm_header_t headers[LLM_MAX_HEADERS];
    uint8_t n_headers;
    const char *body;
    size_t body_len;
    uint32_t timeout_ms;
    volatile bool *cancel;
} llm_http_req_t;

typedef struct {
    void (*on_status)(void *u, int status);
    void (*on_header)(void *u, const char *k, const char *v);
    /** Return non-zero to abort the transfer. */
    int (*on_data)(void *u, const char *d, size_t n);
    void *u;
} llm_http_sink_t;

typedef enum {
    LLM_TR_OK = 0,
    LLM_TR_CONFIG,    /* bad URL, plain http with API key, etc — not retryable */
    LLM_TR_CONNECT,   /* DNS/TCP connect failed — request never sent */
    LLM_TR_TLS,       /* handshake/verification failed — not retried */
    LLM_TR_TIMEOUT,
    LLM_TR_IO,        /* connection dropped mid-exchange */
    LLM_TR_CANCELED,
    LLM_TR_ABORTED,   /* sink requested abort */
    LLM_TR_NO_NETWORK,
} llm_tr_err_t;

const char *llm_tr_str(llm_tr_err_t e);

typedef struct llm_transport {
    llm_tr_err_t (*perform)(struct llm_transport *t, const llm_http_req_t *req, const llm_http_sink_t *sink);
    void *ctx;
} llm_transport_t;

/* ------------------------------------------------------------ config */

typedef enum { LLM_KIND_OPENAI = 0, LLM_KIND_ANTHROPIC = 1, LLM_KIND_COUNT } llm_kind_t;

typedef struct {
    llm_kind_t kind;
    char base_url[128];   /* e.g. http://192.168.1.20:8080/v1 or https://api.anthropic.com */
    char model[64];
    char api_key[200];    /* SECRET — never logged; stored in the secrets namespace */
    uint16_t max_tokens;
    float temperature;    /* <0 → omitted */
    bool stream;
    uint32_t timeout_ms;
    bool allow_missing_ids;  /* compat: synthesise ids for servers that omit them */
    bool prompt_cache;       /* Anthropic: cache_control on tools/system */
} llm_cfg_t;

void llm_cfg_default(llm_cfg_t *c, llm_kind_t kind);

/* ------------------------------------------------------------ provider */

typedef struct {
    bool tools;
    bool streaming;
    bool parallel_tool_calls;
    bool requires_key;
    bool native_tool_ids;
    uint32_t max_context_tokens; /* 0 = unknown / server-defined */
} llm_caps_t;

typedef struct {
    const char *system_prompt;
    const llm_conv_t *conv;
    const tool_registry_t *tools;
    tool_source_t src;
} llm_req_ctx_t;

struct llm_provider;

typedef struct llm_parser {
    const struct llm_provider *prov;
    const llm_cfg_t *cfg;
    llm_response_t *resp;
    bool stream;
    int http_status;
    uint32_t retry_after_ms;
    /* body (non-stream) or current SSE line (stream) */
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
    char event[32];
    lla_jtok_t *toks;
    uint16_t max_toks;
    int err;          /* first llm_perr_t error, sticky */
    bool finished;    /* terminal stop info seen */
    bool done_marker; /* [DONE] / message_stop */
    bool any_data;
    int8_t block_call[16]; /* anthropic: content block index → call slot */
    int8_t idx_call[LLM_MAX_CALLS * 2]; /* openai: tool_call index → call slot */
} llm_parser_t;

typedef struct llm_provider {
    const char *name;
    llm_kind_t kind;
    llm_caps_t caps;
    const char *chat_path;   /* appended to base_url */
    const char *models_path;
    /** Build request body. Returns false on overflow. */
    bool (*build)(const llm_cfg_t *cfg, const llm_req_ctx_t *ctx, lla_jw_t *w);
    /** Fill headers. Secret header values are formatted into @p scratch,
     *  which the caller wipes after the request. Returns count. */
    uint8_t (*headers)(const llm_cfg_t *cfg, llm_header_t *h, uint8_t max, char *scratch, size_t scratch_cap);
    /** Process one complete SSE data payload (stream) */
    void (*on_sse)(llm_parser_t *p, const char *data, size_t n);
    /** Process the full body (non-stream, HTTP 200) */
    void (*on_body)(llm_parser_t *p, const char *body, size_t n);
} llm_provider_t;

extern const llm_provider_t llm_provider_openai;
extern const llm_provider_t llm_provider_anthropic;

/** Registry so new providers can be added without touching the agent core. */
const llm_provider_t *llm_provider_get(llm_kind_t kind);
const llm_provider_t *llm_provider_by_name(const char *name);

/* ------------------------------------------------------- parser driver */

void llm_parser_init(llm_parser_t *p, const llm_provider_t *prov, const llm_cfg_t *cfg, llm_response_t *resp,
                     char *buf, size_t cap, lla_jtok_t *toks, uint16_t max_toks);
void llm_parser_status(llm_parser_t *p, int status);
void llm_parser_header(llm_parser_t *p, const char *k, const char *v);
/** Feed raw body bytes in arbitrary chunks. Returns 0 or llm_perr_t. */
int llm_parser_feed(llm_parser_t *p, const char *d, size_t n);
/** Finish: validates completeness and every tool call. Returns llm_perr_t. */
int llm_parser_finish(llm_parser_t *p);

/** HTTP sink adapter that forwards into a parser. */
void llm_parser_sink(llm_parser_t *p, llm_http_sink_t *sink);

/* -------------------------------------------- helpers used by providers */

void llm_resp_reset(llm_response_t *r);
void llm_append_text(llm_parser_t *p, const char *js, const lla_jtok_t *t);
void llm_set_err(llm_parser_t *p, int err);
/** Append decoded JSON string token to fixed buffer; sets *overflow. */
bool llm_append_tok(const char *js, const lla_jtok_t *t, char *dst, size_t cap, uint16_t *len);
void llm_copy_tok(const char *js, const lla_jtok_t *t, char *dst, size_t cap, bool *overflow);
/** Parse {"error":{...}} bodies of both providers into resp->err_*. */
void llm_parse_error_body(llm_parser_t *p, const char *js, size_t n);
/** Parse a /models listing; calls cb for each id. Returns count or <0. */
int llm_parse_models(const char *js, size_t n, lla_jtok_t *toks, uint16_t max_toks,
                     void (*cb)(void *u, const char *id), void *u);

/**
 * Streaming extractor for model listings: emits every "id":"..." value as
 * bytes arrive, so listings of any size work with a few bytes of state
 * (Anthropic's /v1/models is larger than the console buffer).
 */
typedef struct {
    void (*cb)(void *u, const char *id);
    void *u;
    char win[4];   /* last 4 bytes, to spot the "id" key */
    uint8_t st;    /* 0 scan, 1 expect ':', 2 expect '"', 3 in value, 4 escape */
    char id[96];
    uint8_t n;
    bool clipped;
    int count;
} llm_id_scan_t;

void llm_id_scan_init(llm_id_scan_t *s, void (*cb)(void *u, const char *id), void *u);
void llm_id_scan_feed(llm_id_scan_t *s, const char *d, size_t n);

/** Build full URL "<base><path>" into out; false if it does not fit. */
bool llm_join_url(char *out, size_t cap, const char *base, const char *path);

#ifdef __cplusplus
}
#endif
