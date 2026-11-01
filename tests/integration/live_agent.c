/*
 * live_agent.c — runs the real agent loop + protocol parsers against a live
 * LLM server from the host, with mock tools. Verifies protocol compatibility
 * with real servers; it is NOT hardware validation.
 *
 *   live_agent <openai|anthropic> <base_url> <model> [stream 0|1]
 *   API key (if any) is read from the LLA_API_KEY environment variable.
 *
 * Transport: minimal HTTP/1.1 over POSIX sockets (http:// only) with chunked
 * decoding — enough for local servers such as llama-server, Ollama, LM Studio.
 */
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "agent.h"

/* ------------------------------------------------------------ port */

uint64_t lla_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
void lla_sleep_ms(uint32_t ms) { usleep(ms * 1000); }
uint32_t lla_rand32(void) { return (uint32_t)rand(); }
void lla_log(lla_log_level_t lvl, const char *tag, const char *fmt, ...)
{
    if (lvl > LLA_LOG_WARN) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
void lla_lock_init(lla_lock_t *l) { (void)l; }
void lla_lock(lla_lock_t *l) { (void)l; }
void lla_unlock(lla_lock_t *l) { (void)l; }

/* ------------------------------------------------------------ socket transport */

static int sink_body(const llm_http_sink_t *s, const char *d, size_t n) { return s->on_data(s->u, d, n); }

static llm_tr_err_t sock_perform(llm_transport_t *t, const llm_http_req_t *req, const llm_http_sink_t *sink)
{
    (void)t;
    char host[128], port[8] = "80";
    const char *p = req->url;
    if (strncmp(p, "http://", 7) != 0) return LLM_TR_CONFIG;
    p += 7;
    const char *slash = strchr(p, '/');
    size_t hl = (size_t)(slash - p);
    snprintf(host, sizeof(host), "%.*s", (int)hl, p);
    char *colon = strchr(host, ':');
    if (colon) {
        snprintf(port, sizeof(port), "%s", colon + 1);
        *colon = 0;
    }
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *ai;
    if (getaddrinfo(host, port, &hints, &ai) != 0) return LLM_TR_CONNECT;
    int fd = socket(ai->ai_family, ai->ai_socktype, 0);
    if (fd < 0 || connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        freeaddrinfo(ai);
        if (fd >= 0) close(fd);
        return LLM_TR_CONNECT;
    }
    freeaddrinfo(ai);
    struct timeval tv = {.tv_sec = req->timeout_ms / 1000 + 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr), "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: %zu\r\n",
                     req->method, slash, host, req->body_len);
    for (int i = 0; i < req->n_headers; i++)
        n += snprintf(hdr + n, sizeof(hdr) - (size_t)n, "%s: %s\r\n", req->headers[i].name, req->headers[i].value);
    n += snprintf(hdr + n, sizeof(hdr) - (size_t)n, "\r\n");
    if (write(fd, hdr, (size_t)n) != n || write(fd, req->body, req->body_len) != (ssize_t)req->body_len) {
        close(fd);
        return LLM_TR_IO;
    }
    static char buf[65536];
    size_t len = 0;
    ssize_t r;
    char *body = NULL;
    while (!body && (r = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += (size_t)r;
        buf[len] = 0;
        body = strstr(buf, "\r\n\r\n");
    }
    if (!body) {
        close(fd);
        return LLM_TR_IO;
    }
    *body = 0;
    body += 4;
    sink->on_status(sink->u, atoi(buf + 9));
    bool chunked = strcasestr(buf, "transfer-encoding: chunked") != NULL;
    char *ra = strcasestr(buf, "retry-after:");
    if (ra) sink->on_header(sink->u, "Retry-After", ra + 12);
    size_t have = len - (size_t)(body - buf);
    memmove(buf, body, have);
    len = have;
    llm_tr_err_t res = LLM_TR_OK;
    if (!chunked) {
        if (len && sink_body(sink, buf, len)) res = LLM_TR_ABORTED;
        while (res == LLM_TR_OK && (r = read(fd, buf, sizeof(buf))) > 0)
            if (sink_body(sink, buf, (size_t)r)) res = LLM_TR_ABORTED;
        if (r < 0 && res == LLM_TR_OK) res = LLM_TR_TIMEOUT;
    } else {
        for (;;) {
            char *eol;
            while (!(eol = memmem(buf, len, "\r\n", 2))) {
                r = read(fd, buf + len, sizeof(buf) - len);
                if (r <= 0) {
                    close(fd);
                    return LLM_TR_IO;
                }
                len += (size_t)r;
            }
            size_t csz = strtoul(buf, NULL, 16);
            size_t hdrlen = (size_t)(eol - buf) + 2;
            if (csz == 0) break;
            while (len < hdrlen + csz + 2) {
                r = read(fd, buf + len, sizeof(buf) - len);
                if (r <= 0) {
                    close(fd);
                    return LLM_TR_IO;
                }
                len += (size_t)r;
            }
            if (sink_body(sink, buf + hdrlen, csz)) {
                res = LLM_TR_ABORTED;
                break;
            }
            memmove(buf, buf + hdrlen + csz + 2, len - hdrlen - csz - 2);
            len -= hdrlen + csz + 2;
        }
    }
    close(fd);
    return res;
}

/* ------------------------------------------------------------ mock tools */

static int s_fan = 0, s_reads = 0, s_sets = 0;
static const tool_arg_def_t A_DEV[] = {{.name = "device", .type = TARG_STR, .required = true, .max_len = 15}};
static const tool_arg_def_t A_ACT[] = {{.name = "device", .type = TARG_STR, .required = true, .max_len = 15},
                                       {.name = "value", .desc = "1 = on, 0 = off", .type = TARG_INT, .required = true,
                                        .min = 0, .max = 1}};

static void x_caps(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)d; (void)a; (void)dl;
    lla_jw_key(&r->w, "devices");
    lla_jw_raw(&r->w, "[{\"name\":\"room_temp\",\"type\":\"sensor\",\"unit\":\"C\"},"
                      "{\"name\":\"cooling_fan\",\"type\":\"switch\",\"agent_writable\":true}]", 110);
}
static void x_read(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    s_reads++;
    if (strcmp(tool_arg_str(d, a, "device", ""), "room_temp")) {
        tool_result_fail(r, TOOL_ST_REJECTED, "device_not_found", NULL);
        return;
    }
    lla_jw_kv_num(&r->w, "value", 29.5);
    lla_jw_kv_str(&r->w, "unit", "C");
}
static void x_set(const tool_def_t *d, const tool_args_t *a, uint64_t dl, tool_result_t *r)
{
    (void)dl;
    s_sets++;
    if (strcmp(tool_arg_str(d, a, "device", ""), "cooling_fan")) {
        tool_result_fail(r, TOOL_ST_REJECTED, "device_not_found", NULL);
        return;
    }
    s_fan = (int)tool_arg_int(d, a, "value", 0);
    lla_jw_kv_int(&r->w, "value", s_fan);
}
static bool v_set(const tool_def_t *d, const tool_args_t *a, tool_result_t *r)
{
    (void)r;
    return s_fan == tool_arg_int(d, a, "value", -1);
}

static const tool_def_t k_tools[] = {
    {.name = "get_registered_capabilities", .desc = "List devices.", .perm = TOOL_PERM_READ, .exec = x_caps},
    {.name = "read_sensor", .desc = "Read a sensor by name.", .args = A_DEV, .n_args = 1, .perm = TOOL_PERM_READ,
     .exec = x_read},
    {.name = "set_actuator", .desc = "Switch an actuator on (1) or off (0).", .args = A_ACT, .n_args = 2,
     .perm = TOOL_PERM_ACTUATE, .effect = TOOL_EFFECT_IDEMPOTENT, .exec = x_set, .verify = v_set},
};

static void on_state(void *u, agent_state_t s, const char *d)
{
    (void)u;
    fprintf(stderr, "  -> %s%s%s\n", agent_state_str(s), d ? " " : "", d ? d : "");
}
static void on_tool(void *u, const char *n, const char *a, const tool_result_t *r)
{
    (void)u;
    fprintf(stderr, "  [tool] %s %s -> %s %s\n", n, a, tool_status_str(r->status), r->json);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <openai|anthropic> <base_url> <model> [stream 0|1]\n", argv[0]);
        return 2;
    }
    static char req[32768], resp[16384];
    static lla_jtok_t toks[1024];
    static uint8_t conv[16384];
    static tool_registry_t reg;
    static agent_t ag;
    static llm_transport_t tr = {.perform = sock_perform};
    tool_registry_init(&reg);
    for (size_t i = 0; i < 3; i++) tool_registry_add(&reg, &k_tools[i]);
    agent_mem_t m = {req, sizeof(req), resp, sizeof(resp), toks, 1024, conv, sizeof(conv)};
    agent_init(&ag, &m, &tr, &reg);
    llm_cfg_t c;
    const llm_provider_t *p = llm_provider_by_name(argv[1]);
    if (!p) return 2;
    llm_cfg_default(&c, p->kind);
    snprintf(c.base_url, sizeof(c.base_url), "%s", argv[2]);
    snprintf(c.model, sizeof(c.model), "%s", argv[3]);
    c.stream = argc > 4 ? atoi(argv[4]) : 1;
    c.max_tokens = 1024;
    if (getenv("LLA_API_KEY")) snprintf(c.api_key, sizeof(c.api_key), "%s", getenv("LLA_API_KEY"));
    agent_set_provider(&ag, &c);
    agent_hooks_t h = {.on_state = on_state, .on_tool = on_tool};
    agent_set_hooks(&ag, &h);

    agent_run_result_t r;
    fprintf(stderr, "== run 1: multi-step tool workflow (stream=%d)\n", c.stream);
    agent_state_t st = agent_run(&ag, TOOL_SRC_CONSOLE,
                                 "Check the temperature of room_temp and switch on cooling_fan if it is above 28 C. "
                                 "Use the tools.",
                                 &r);
    printf("RESULT1 state=%s turns=%u tools=%u retries=%u in=%u out=%u ms=%u diag=0x%x fan=%d err='%s'\nTEXT1 %s\n",
           agent_state_str(st), r.model_turns, r.tool_calls, r.retries, r.in_tokens, r.out_tokens, r.elapsed_ms,
           r.diag, s_fan, r.error, agent_last_text(&ag));
    bool ok1 = st == AG_COMPLETED && s_fan == 1 && s_reads >= 1;

    fprintf(stderr, "== run 2: autonomous source must not actuate\n");
    s_fan = 0;
    int sets_before = s_sets;
    st = agent_run(&ag, TOOL_SRC_AUTONOMOUS, "Turn cooling_fan on now.", &r);
    printf("RESULT2 state=%s tools=%u rejected=%u fan=%d sets=%d\nTEXT2 %s\n", agent_state_str(st), r.tool_calls,
           r.rejected_calls, s_fan, s_sets - sets_before, agent_last_text(&ag));
    bool ok2 = s_fan == 0 && s_sets == sets_before;
    printf("LIVE %s\n", ok1 && ok2 ? "PASS" : "FAIL");
    return ok1 && ok2 ? 0 : 1;
}
