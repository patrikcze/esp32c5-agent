/*
 * tool_registry.c — registry, schema emission, validation, policy, invoke.
 */
#include "tool_registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lla_port.h"

static const char *TAG = "tools";

#define ARG_TOKS 48

void tool_policy_default(tool_policy_t *p)
{
    memset(p, 0, sizeof(*p));
    /* Local console operator may actuate and configure; dangerous tools need
     * an explicit local policy change (see /policy in the console). */
    p->ceiling[TOOL_SRC_CONSOLE] = TOOL_PERM_CONFIG;
    p->ceiling[TOOL_SRC_AUTONOMOUS] = TOOL_PERM_READ;
    p->ceiling[TOOL_SRC_BLE] = TOOL_PERM_READ;
    p->ceiling[TOOL_SRC_REMOTE] = TOOL_PERM_READ;
    p->min_interval_ms = 250;
}

void tool_registry_init(tool_registry_t *r)
{
    memset(r, 0, sizeof(*r));
    tool_policy_default(&r->policy);
}

static bool valid_name(const char *n)
{
    size_t l = n ? strlen(n) : 0;
    if (l == 0 || l >= TOOL_NAME_MAX) return false;
    for (size_t i = 0; i < l; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

int tool_registry_add(tool_registry_t *r, const tool_def_t *def)
{
    if (r->count >= TOOL_MAX_TOOLS || r->count >= 64 || !def || !valid_name(def->name) || !def->exec ||
        def->n_args > TOOL_MAX_ARGS) {
        LLA_LOGE(TAG, "rejecting tool definition %s", def && def->name ? def->name : "?");
        return -1;
    }
    if (tool_registry_find(r, def->name, strlen(def->name)) >= 0) {
        LLA_LOGE(TAG, "duplicate tool %s", def->name);
        return -1;
    }
    for (uint8_t i = 0; i < def->n_args; i++) {
        if (!valid_name(def->args[i].name)) return -1;
        if (def->args[i].type == TARG_ENUM && !def->args[i].enum_vals) return -1;
    }
    r->tools[r->count] = def;
    return r->count++;
}

int tool_registry_find(const tool_registry_t *r, const char *name, size_t name_len)
{
    for (int i = 0; i < r->count; i++) {
        const char *n = r->tools[i]->name;
        if (strlen(n) == name_len && memcmp(n, name, name_len) == 0) return i;
    }
    return -1;
}

bool tool_registry_permitted(const tool_registry_t *r, int idx, tool_source_t src)
{
    if (idx < 0 || idx >= r->count || src >= TOOL_SRC_COUNT) return false;
    if (r->policy.deny_mask & (1ULL << idx)) return false;
    if (r->tools[idx]->perm <= r->policy.ceiling[src]) return true;
    if (src == TOOL_SRC_AUTONOMOUS && (r->policy.allow_autonomous_mask & (1ULL << idx)) &&
        r->tools[idx]->perm <= TOOL_PERM_ACTUATE) {
        return true;
    }
    return false;
}

static void write_params(const tool_def_t *t, lla_jw_t *w)
{
    lla_jw_obj_begin(w);
    lla_jw_kv_str(w, "type", "object");
    lla_jw_key(w, "properties");
    lla_jw_obj_begin(w);
    for (uint8_t i = 0; i < t->n_args; i++) {
        const tool_arg_def_t *a = &t->args[i];
        lla_jw_key(w, a->name);
        lla_jw_obj_begin(w);
        switch (a->type) {
        case TARG_INT:
            lla_jw_kv_str(w, "type", "integer");
            lla_jw_kv_num(w, "minimum", a->min);
            lla_jw_kv_num(w, "maximum", a->max);
            break;
        case TARG_NUM:
            lla_jw_kv_str(w, "type", "number");
            lla_jw_kv_num(w, "minimum", a->min);
            lla_jw_kv_num(w, "maximum", a->max);
            break;
        case TARG_BOOL:
            lla_jw_kv_str(w, "type", "boolean");
            break;
        case TARG_STR:
            lla_jw_kv_str(w, "type", "string");
            lla_jw_kv_int(w, "maxLength", a->max_len);
            break;
        case TARG_ENUM:
            lla_jw_kv_str(w, "type", "string");
            lla_jw_key(w, "enum");
            lla_jw_arr_begin(w);
            for (const char *const *e = a->enum_vals; *e; e++) lla_jw_str(w, *e);
            lla_jw_arr_end(w);
            break;
        }
        if (a->desc) lla_jw_kv_str(w, "description", a->desc);
        lla_jw_obj_end(w);
    }
    lla_jw_obj_end(w);
    lla_jw_key(w, "required");
    lla_jw_arr_begin(w);
    for (uint8_t i = 0; i < t->n_args; i++) {
        if (t->args[i].required) lla_jw_str(w, t->args[i].name);
    }
    lla_jw_arr_end(w);
    lla_jw_kv_bool(w, "additionalProperties", false);
    lla_jw_obj_end(w);
}

void tool_registry_write_schemas(const tool_registry_t *r, tool_source_t src, tool_schema_fmt_t fmt, lla_jw_t *w)
{
    lla_jw_arr_begin(w);
    for (int i = 0; i < r->count; i++) {
        if (!tool_registry_permitted(r, i, src)) continue;
        const tool_def_t *t = r->tools[i];
        lla_jw_obj_begin(w);
        if (fmt == TOOL_FMT_OPENAI) {
            lla_jw_kv_str(w, "type", "function");
            lla_jw_key(w, "function");
            lla_jw_obj_begin(w);
            lla_jw_kv_str(w, "name", t->name);
            lla_jw_kv_str(w, "description", t->desc);
            lla_jw_key(w, "parameters");
            write_params(t, w);
            lla_jw_obj_end(w);
        } else {
            lla_jw_kv_str(w, "name", t->name);
            lla_jw_kv_str(w, "description", t->desc);
            lla_jw_key(w, "input_schema");
            write_params(t, w);
        }
        lla_jw_obj_end(w);
    }
    lla_jw_arr_end(w);
}

int tool_arg_index(const tool_def_t *def, const char *name)
{
    for (uint8_t i = 0; i < def->n_args; i++) {
        if (strcmp(def->args[i].name, name) == 0) return i;
    }
    return -1;
}

#define WHY(...) snprintf(why, why_cap, __VA_ARGS__)

bool tool_args_parse(const tool_def_t *def, const char *json, size_t len, tool_args_t *out, char *why,
                     size_t why_cap)
{
    memset(out, 0, sizeof(*out));
    lla_jtok_t toks[ARG_TOKS];
    /* Some servers send "" for no-argument calls; treat as {} */
    if (len == 0) {
        json = "{}";
        len = 2;
    }
    int n = lla_json_parse(json, len, toks, ARG_TOKS);
    if (n < 0) {
        WHY("arguments are not valid JSON (err %d)", n);
        return false;
    }
    if (toks[0].type != LLA_JT_OBJECT) {
        WHY("arguments must be a JSON object");
        return false;
    }
    int i = 1;
    for (uint16_t k = 0; k < toks[0].size; k++) {
        const lla_jtok_t *key = &toks[i];
        int v = i + 1;
        int ai = -1;
        for (uint8_t j = 0; j < def->n_args; j++) {
            if (lla_json_streq(json, key, def->args[j].name)) {
                ai = j;
                break;
            }
        }
        if (ai < 0) {
            WHY("unknown argument '%.*s'", (int)(lla_json_len(key) > 24 ? 24 : lla_json_len(key)), json + key->start);
            return false;
        }
        const tool_arg_def_t *ad = &def->args[ai];
        tool_arg_val_t *av = &out->a[ai];
        if (av->present) {
            WHY("duplicate argument '%s'", ad->name);
            return false;
        }
        av->present = true;
        av->type = ad->type;
        const lla_jtok_t *vt = &toks[v];
        switch (ad->type) {
        case TARG_INT: {
            int64_t x;
            if (!lla_json_int(json, vt, &x)) {
                WHY("'%s' must be an integer", ad->name);
                return false;
            }
            if ((double)x < ad->min || (double)x > ad->max) {
                WHY("'%s' out of range [%g,%g]", ad->name, ad->min, ad->max);
                return false;
            }
            av->v.i = x;
            break;
        }
        case TARG_NUM: {
            double x;
            if (!lla_json_num(json, vt, &x)) {
                WHY("'%s' must be a number", ad->name);
                return false;
            }
            if (x < ad->min || x > ad->max) {
                WHY("'%s' out of range [%g,%g]", ad->name, ad->min, ad->max);
                return false;
            }
            av->v.d = x;
            break;
        }
        case TARG_BOOL:
            if (vt->type != LLA_JT_TRUE && vt->type != LLA_JT_FALSE) {
                WHY("'%s' must be a boolean", ad->name);
                return false;
            }
            av->v.b = vt->type == LLA_JT_TRUE;
            break;
        case TARG_STR:
        case TARG_ENUM: {
            if (vt->type != LLA_JT_STRING) {
                WHY("'%s' must be a string", ad->name);
                return false;
            }
            char *dst = out->pool + out->pool_used;
            size_t room = sizeof(out->pool) - out->pool_used;
            int l = lla_json_str(json, vt, dst, room);
            if (l < 0) {
                WHY("'%s' too long", ad->name);
                return false;
            }
            if (ad->type == TARG_STR && ad->max_len && (size_t)l > ad->max_len) {
                WHY("'%s' longer than %u", ad->name, ad->max_len);
                return false;
            }
            if (strlen(dst) != (size_t)l) {
                WHY("'%s' contains NUL", ad->name);
                return false;
            }
            if (ad->type == TARG_ENUM) {
                bool ok = false;
                for (const char *const *e = ad->enum_vals; *e; e++) {
                    if (strcmp(*e, dst) == 0) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    WHY("'%s' has invalid value", ad->name);
                    return false;
                }
            }
            av->v.s = dst;
            out->pool_used += (uint16_t)(l + 1);
            break;
        }
        }
        i = lla_json_next(toks, v);
    }
    for (uint8_t j = 0; j < def->n_args; j++) {
        if (def->args[j].required && !out->a[j].present) {
            WHY("missing required argument '%s'", def->args[j].name);
            return false;
        }
    }
    return true;
}

bool tool_arg_present(const tool_def_t *def, const tool_args_t *a, const char *name)
{
    int i = tool_arg_index(def, name);
    return i >= 0 && a->a[i].present;
}

int64_t tool_arg_int(const tool_def_t *def, const tool_args_t *a, const char *name, int64_t dflt)
{
    int i = tool_arg_index(def, name);
    return (i >= 0 && a->a[i].present && a->a[i].type == TARG_INT) ? a->a[i].v.i : dflt;
}

double tool_arg_num(const tool_def_t *def, const tool_args_t *a, const char *name, double dflt)
{
    int i = tool_arg_index(def, name);
    if (i < 0 || !a->a[i].present) return dflt;
    if (a->a[i].type == TARG_NUM) return a->a[i].v.d;
    if (a->a[i].type == TARG_INT) return (double)a->a[i].v.i;
    return dflt;
}

bool tool_arg_bool(const tool_def_t *def, const tool_args_t *a, const char *name, bool dflt)
{
    int i = tool_arg_index(def, name);
    return (i >= 0 && a->a[i].present && a->a[i].type == TARG_BOOL) ? a->a[i].v.b : dflt;
}

const char *tool_arg_str(const tool_def_t *def, const tool_args_t *a, const char *name, const char *dflt)
{
    int i = tool_arg_index(def, name);
    if (i >= 0 && a->a[i].present && (a->a[i].type == TARG_STR || a->a[i].type == TARG_ENUM)) return a->a[i].v.s;
    return dflt;
}

void tool_result_begin(tool_result_t *res)
{
    res->status = TOOL_ST_OK;
    res->verified = false;
    lla_jw_init(&res->w, res->json, sizeof(res->json));
    lla_jw_obj_begin(&res->w);
}

void tool_result_fail(tool_result_t *res, tool_status_t st, const char *code, const char *detail)
{
    tool_result_begin(res);
    res->status = st;
    lla_jw_kv_str(&res->w, "error", code);
    if (detail && detail[0]) lla_jw_kv_str(&res->w, "detail", detail);
}

void tool_result_end(tool_result_t *res)
{
    lla_jw_obj_end(&res->w);
    if (!lla_jw_ok(&res->w)) {
        lla_jw_init(&res->w, res->json, sizeof(res->json));
        lla_jw_obj_begin(&res->w);
        lla_jw_kv_str(&res->w, "status", tool_status_str(res->status));
        lla_jw_kv_bool(&res->w, "result_truncated", true);
        lla_jw_obj_end(&res->w);
    }
}

const char *tool_status_str(tool_status_t s)
{
    static const char *const n[] = {"ok", "error", "rejected", "timeout", "unknown_outcome"};
    return (unsigned)s < 5 ? n[s] : "?";
}

const char *tool_perm_str(tool_perm_t p)
{
    static const char *const n[] = {"read", "actuate", "config", "dangerous"};
    return (unsigned)p < 4 ? n[p] : "?";
}

const char *tool_source_str(tool_source_t s)
{
    static const char *const n[] = {"console", "autonomous", "ble", "remote"};
    return (unsigned)s < TOOL_SRC_COUNT ? n[s] : "?";
}

void tool_registry_invoke(tool_registry_t *r, int idx, tool_source_t src, const char *args_json, size_t args_len,
                          uint64_t now_ms, tool_result_t *res, void (*before_verify)(void *u), void *u)
{
    if (idx < 0 || idx >= r->count) {
        tool_result_fail(res, TOOL_ST_REJECTED, "unknown_tool", NULL);
        tool_result_end(res);
        return;
    }
    const tool_def_t *t = r->tools[idx];
    if (!tool_registry_permitted(r, idx, src)) {
        char d[64];
        snprintf(d, sizeof(d), "tool requires '%s' permission, source '%s' not allowed", tool_perm_str(t->perm),
                 tool_source_str(src));
        tool_result_fail(res, TOOL_ST_REJECTED, "permission_denied", d);
        tool_result_end(res);
        LLA_LOGW(TAG, "policy rejected %s for %s", t->name, tool_source_str(src));
        return;
    }
    tool_args_t args;
    char why[80];
    if (!tool_args_parse(t, args_json, args_len, &args, why, sizeof(why))) {
        tool_result_fail(res, TOOL_ST_REJECTED, "invalid_arguments", why);
        tool_result_end(res);
        LLA_LOGW(TAG, "invalid args for %s: %s", t->name, why);
        return;
    }
    if (t->effect != TOOL_EFFECT_NONE && r->policy.min_interval_ms && r->last_side_effect_ms[idx] &&
        now_ms - r->last_side_effect_ms[idx] < r->policy.min_interval_ms) {
        tool_result_fail(res, TOOL_ST_REJECTED, "rate_limited", "called too frequently");
        tool_result_end(res);
        return;
    }
    tool_result_begin(res);
    uint64_t deadline = now_ms + (t->timeout_ms ? t->timeout_ms : 1000);
    t->exec(t, &args, deadline, res);
    if (t->effect != TOOL_EFFECT_NONE && res->status != TOOL_ST_REJECTED) {
        r->last_side_effect_ms[idx] = now_ms ? now_ms : 1;
    }
    if (res->status == TOOL_ST_OK && t->verify) {
        if (before_verify) before_verify(u);
        res->verified = t->verify(t, &args, res);
        lla_jw_kv_bool(&res->w, "verified", res->verified);
        if (!res->verified) {
            /* The command was issued but the readback disagrees: we cannot
             * say whether the effect happened, so never auto-repeat it. */
            res->status = TOOL_ST_UNKNOWN;
            lla_jw_kv_str(&res->w, "warning", "postcondition_failed");
        }
    }
    tool_result_end(res);
}
