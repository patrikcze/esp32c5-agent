/*
 * http_transport_esp.c — llm_transport_t over esp_http_client.
 *
 * - TLS always verifies the server: ESP x509 certificate bundle, or a
 *   configured private CA (PEM) for local inference servers. There is no
 *   option to skip verification.
 * - Plain http:// is only allowed when CONFIG_LLA_ALLOW_PLAIN_HTTP is set
 *   AND the request carries no credentials (no Authorization / x-api-key).
 * - One persistent client is reused while scheme/host/port stay the same
 *   (keep-alive); any error closes it so the next request starts clean.
 * - The body is streamed to the sink in 1 KB chunks; nothing is buffered
 *   here beyond esp_http_client's own fixed buffers.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net.h"
#include "sdkconfig.h"

static const char *TAG = "http";

net_status_t *net_stats_mut(void);

static esp_http_client_handle_t s_cli;
static char s_origin[96];     /* scheme://host:port of s_cli */
static char *s_ca_pem;        /* heap copy of private CA, NULL = bundle */
static const llm_http_sink_t *s_sink;
#define LLM_TR_STALE ((llm_tr_err_t)100) /* internal: reused socket dead before response */
static char s_rx[1024];

int net_set_ca_pem(const char *pem)
{
    free(s_ca_pem);
    s_ca_pem = NULL;
    if (pem && pem[0]) {
        if (!strstr(pem, "-----BEGIN CERTIFICATE-----")) return -1;
        s_ca_pem = strdup(pem);
        if (!s_ca_pem) return -2;
    }
    if (s_cli) { /* force a new handshake with the new trust anchor */
        esp_http_client_cleanup(s_cli);
        s_cli = NULL;
    }
    return 0;
}

bool net_has_custom_ca(void) { return s_ca_pem != NULL; }

const char *net_custom_ca(void) { return s_ca_pem; }

static bool origin_of(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    if (!p) return false;
    p += 3;
    const char *e = strchr(p, '/');
    size_t n = e ? (size_t)(e - url) : strlen(url);
    if (n >= cap) return false;
    memcpy(out, url, n);
    out[n] = '\0';
    return true;
}

static esp_err_t on_http_event(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_HEADER && s_sink && s_sink->on_header) {
        s_sink->on_header(s_sink->u, ev->header_key, ev->header_value);
    }
    return ESP_OK;
}

static void drop_client(void)
{
    if (s_cli) {
        esp_http_client_cleanup(s_cli);
        s_cli = NULL;
    }
    s_origin[0] = '\0';
}

static llm_tr_err_t classify_open_error(esp_err_t e)
{
    int tls_code = 0, tls_flags = 0;
    if (s_cli) esp_http_client_get_and_clear_last_tls_error(s_cli, &tls_code, &tls_flags);
    if (tls_flags != 0 || (tls_code != 0 && e != ESP_ERR_HTTP_CONNECT)) {
        net_stats_mut()->tls_failures++;
        ESP_LOGE(TAG, "TLS failure (esp-tls 0x%x, verify flags 0x%x): check CA / time", tls_code, tls_flags);
        return LLM_TR_TLS;
    }
    if (e == ESP_ERR_HTTP_EAGAIN || e == ESP_ERR_TIMEOUT) return LLM_TR_TIMEOUT;
    return LLM_TR_CONNECT;
}

static llm_tr_err_t perform_locked(const llm_http_req_t *req, const llm_http_sink_t *sink);

/* One client is shared by the agent task and console commands. */
static llm_tr_err_t perform(llm_transport_t *t, const llm_http_req_t *req, const llm_http_sink_t *sink)
{
    (void)t;
    static SemaphoreHandle_t mtx;
    if (!mtx) mtx = xSemaphoreCreateMutex();
    if (xSemaphoreTake(mtx, pdMS_TO_TICKS(req->timeout_ms)) != pdTRUE) return LLM_TR_TIMEOUT;
    llm_tr_err_t r = perform_locked(req, sink);
    if (r == LLM_TR_STALE) {
        /* A kept-alive connection was closed by the server while idle. The
         * request did not reach it (failed before any response byte), so
         * one immediate retry on a fresh connection is safe. */
        ESP_LOGI(TAG, "reused connection was closed by the server; reconnecting");
        r = perform_locked(req, sink);
        if (r == LLM_TR_STALE) r = LLM_TR_IO;
    }
    xSemaphoreGive(mtx);
    return r;
}

static llm_tr_err_t perform_locked(const llm_http_req_t *req, const llm_http_sink_t *sink)
{
    bool https = strncmp(req->url, "https://", 8) == 0;
    bool http = strncmp(req->url, "http://", 7) == 0;
    if (!https && !http) return LLM_TR_CONFIG;
    if (http) {
#if !CONFIG_LLA_ALLOW_PLAIN_HTTP
        ESP_LOGE(TAG, "plain http disabled (CONFIG_LLA_ALLOW_PLAIN_HTTP)");
        return LLM_TR_CONFIG;
#endif
        for (int i = 0; i < req->n_headers; i++) {
            if (!strcasecmp(req->headers[i].name, "Authorization") || !strcasecmp(req->headers[i].name, "x-api-key")) {
                ESP_LOGE(TAG, "refusing to send credentials over plain http");
                return LLM_TR_CONFIG;
            }
        }
    }
    if (!net_wait_ready(0, false)) return LLM_TR_NO_NETWORK;
    if (https && !net_wait_ready(5000, true)) {
        ESP_LOGW(TAG, "time not synchronised: certificate validity cannot be checked");
        return LLM_TR_NO_NETWORK;
    }

    char origin[96];
    if (!origin_of(req->url, origin, sizeof(origin))) return LLM_TR_CONFIG;
    if (s_cli && strcmp(origin, s_origin) != 0) drop_client();
    net_status_t *st = net_stats_mut();
    st->http_requests++;
    bool reused = s_cli != NULL;
    if (!s_cli) {
        esp_http_client_config_t c = {
            .url = req->url,
            .timeout_ms = (int)(req->timeout_ms > 30000 ? 30000 : req->timeout_ms), /* per socket op */
            .event_handler = on_http_event,
            .buffer_size = 1536,
            .buffer_size_tx = 1024,
            .keep_alive_enable = true,
            .disable_auto_redirect = true,
        };
        if (https) {
            if (s_ca_pem) c.cert_pem = s_ca_pem;
            else c.crt_bundle_attach = esp_crt_bundle_attach;
        }
        s_cli = esp_http_client_init(&c);
        if (!s_cli) return LLM_TR_CONNECT;
        strlcpy(s_origin, origin, sizeof(s_origin));
    } else {
        st->http_conn_reused++;
        esp_http_client_set_url(s_cli, req->url);
    }
    esp_http_client_set_method(s_cli, strcmp(req->method, "GET") == 0 ? HTTP_METHOD_GET : HTTP_METHOD_POST);
    esp_http_client_delete_header(s_cli, "Authorization");
    esp_http_client_delete_header(s_cli, "x-api-key");
    for (int i = 0; i < req->n_headers; i++) esp_http_client_set_header(s_cli, req->headers[i].name, req->headers[i].value);

    s_sink = sink;
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_http_client_open(s_cli, (int)req->body_len);
    uint32_t open_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (e != ESP_OK && reused) {
        drop_client();
        s_sink = NULL;
        return LLM_TR_STALE;
    }
    if (e != ESP_OK) {
        llm_tr_err_t r = classify_open_error(e);
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(e));
        drop_client();
        s_sink = NULL;
        return r;
    }
    if (open_ms > 5) { /* new connection (reuse is ~0 ms) */
        st->last_connect_ms = open_ms;
        if (open_ms > st->max_connect_ms) st->max_connect_ms = open_ms;
    }
    /* esp-idf keeps the header values; wipe our scratch copies via the agent */

    size_t sent = 0;
    while (sent < req->body_len) {
        if (req->cancel && *req->cancel) {
            drop_client();
            s_sink = NULL;
            return LLM_TR_CANCELED;
        }
        int w = esp_http_client_write(s_cli, req->body + sent, (int)(req->body_len - sent));
        if (w <= 0) {
            drop_client();
            s_sink = NULL;
            return reused ? LLM_TR_STALE : LLM_TR_IO;
        }
        sent += (size_t)w;
    }
    int64_t cl = esp_http_client_fetch_headers(s_cli);
    if (cl < 0 || (reused && esp_http_client_get_status_code(s_cli) <= 0)) {
        drop_client();
        s_sink = NULL;
        if (reused && cl != -ESP_ERR_HTTP_EAGAIN) return LLM_TR_STALE; /* closed before any response */
        return cl == -ESP_ERR_HTTP_EAGAIN ? LLM_TR_TIMEOUT : LLM_TR_IO;
    }
    int status = esp_http_client_get_status_code(s_cli);
    sink->on_status(sink->u, status);

    uint64_t deadline = (uint64_t)esp_timer_get_time() / 1000 + req->timeout_ms;
    llm_tr_err_t result = LLM_TR_OK;
    for (;;) {
        if (req->cancel && *req->cancel) {
            result = LLM_TR_CANCELED;
            break;
        }
        if ((uint64_t)esp_timer_get_time() / 1000 > deadline) {
            result = LLM_TR_TIMEOUT;
            break;
        }
        int n = esp_http_client_read(s_cli, s_rx, sizeof(s_rx));
        if (n > 0) {
            if (sink->on_data(sink->u, s_rx, (size_t)n)) {
                result = LLM_TR_ABORTED;
                break;
            }
            continue;
        }
        if (n == -ESP_ERR_HTTP_EAGAIN) continue; /* socket timeout slice; overall deadline above */
        if (n == 0 && esp_http_client_is_complete_data_received(s_cli)) break;
        result = LLM_TR_IO; /* connection closed before the body completed */
        break;
    }
    s_sink = NULL;
    if (result != LLM_TR_OK || !esp_http_client_is_persistent_connection(s_cli)) {
        drop_client();
    } else {
        esp_http_client_flush_response(s_cli, NULL);
    }
    return result;
}

static llm_transport_t s_tr = {.perform = perform};

llm_transport_t *net_transport(void) { return &s_tr; }
