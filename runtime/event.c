/*
 * Limceron Event Store — Runtime Instrumentation
 * Ring buffer event store for agent observability.
 * C99, single-threaded (Stage 0).
 */

#include "event.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Static globals ─────────────────────────────────────────────── */

static LcnEventStore   g_event_store;
static LcnAgentRegistry g_agent_registry;

/* ── Helpers ────────────────────────────────────────────────────── */

static uint64_t now_ms(void)
{
    return (uint64_t)time(NULL) * 1000;
}

static void safe_copy(char *dst, const char *src, size_t cap)
{
    if (src) {
        snprintf(dst, cap, "%s", src);
    } else {
        dst[0] = '\0';
    }
}

static const char *agent_status_string(LcnAgentStatus s)
{
    switch (s) {
    case LCN_AGENT_STATUS_RUNNING: return "running";
    case LCN_AGENT_STATUS_PAUSED:  return "paused";
    case LCN_AGENT_STATUS_STOPPED: return "stopped";
    case LCN_AGENT_STATUS_FAILED:  return "failed";
    }
    return "unknown";
}

/* ── Event kind names ───────────────────────────────────────────── */

static const char *g_event_kind_names[LCN_EVENT_KIND_COUNT] = {
    "AGENT_START",
    "AGENT_STOP",
    "AGENT_FAIL",
    "LLM_REQUEST",
    "LLM_RESPONSE",
    "BUDGET_DEDUCT",
    "BUDGET_EXHAUSTED",
    "GUARD_CHECK",
    "GUARD_TRIGGER",
    "CAPABILITY_ACCESS",
    "TOOL_CALL",
    "TOOL_RESULT",
    "CHANNEL_SEND",
    "CHANNEL_RECV",
    "LOG",
    "MEMORY_STORE",
    "MEMORY_QUERY",
    "MEMORY_COMPACT"
};

/* ── Global accessors (auto-init) ───────────────────────────────── */

LcnEventStore *lcn_event_store(void)
{
    if (!g_event_store.initialized) {
        lcn_event_store_init(&g_event_store);
    }
    return &g_event_store;
}

LcnAgentRegistry *lcn_agent_registry(void)
{
    return &g_agent_registry;
}

/* ── Event Store ────────────────────────────────────────────────── */

void lcn_event_store_init(LcnEventStore *store)
{
    memset(store, 0, sizeof(*store));
    store->start_time_ms = now_ms();
    store->initialized = true;
}

uint64_t lcn_event_emit(LcnEventStore *store, LcnEventKind kind,
                          const char *agent_name, const char *session_id,
                          const char *detail_json)
{
    uint64_t idx;
    LcnEvent *ev;
    uint64_t seq;

    if (!store->initialized) {
        lcn_event_store_init(store);
    }

    idx = (store->head + store->count) % LCN_EVENT_STORE_CAPACITY;

    /* If buffer is full, advance head (overwrite oldest) */
    if (store->count == LCN_EVENT_STORE_CAPACITY) {
        store->head = (store->head + 1) % LCN_EVENT_STORE_CAPACITY;
    } else {
        store->count++;
    }

    ev = &store->events[idx];
    seq = store->next_seq++;

    ev->kind = kind;
    ev->timestamp_ms = now_ms();
    ev->seq = seq;
    safe_copy(ev->agent_name, agent_name, LCN_EVENT_AGENT_NAME_MAX);
    safe_copy(ev->session_id, session_id, LCN_EVENT_SESSION_ID_MAX);
    safe_copy(ev->detail_json, detail_json, LCN_EVENT_DETAIL_MAX);

    return seq;
}

int lcn_event_query(const LcnEventStore *store,
                     uint64_t since_seq, int filter_kind,
                     const char *filter_agent, int limit,
                     LcnEvent *out_events, int out_cap)
{
    int matched = 0;
    uint64_t i;
    int max_results;

    if (!store || !out_events || out_cap <= 0) {
        return 0;
    }

    max_results = (limit > 0 && limit < out_cap) ? limit : out_cap;

    for (i = 0; i < store->count && matched < max_results; i++) {
        uint64_t idx = (store->head + i) % LCN_EVENT_STORE_CAPACITY;
        const LcnEvent *ev = &store->events[idx];

        /* Filter by sequence number */
        if (ev->seq < since_seq) {
            continue;
        }

        /* Filter by event kind (-1 means no filter) */
        if (filter_kind >= 0 && (int)ev->kind != filter_kind) {
            continue;
        }

        /* Filter by agent name (NULL means no filter) */
        if (filter_agent && filter_agent[0] != '\0') {
            if (strcmp(ev->agent_name, filter_agent) != 0) {
                continue;
            }
        }

        out_events[matched++] = *ev;
    }

    return matched;
}

uint64_t lcn_event_latest_seq(const LcnEventStore *store)
{
    if (!store || store->count == 0) {
        return 0;
    }
    return store->next_seq - 1;
}

const char *lcn_event_kind_name(LcnEventKind kind)
{
    if ((int)kind >= 0 && kind < LCN_EVENT_KIND_COUNT) {
        return g_event_kind_names[kind];
    }
    return "UNKNOWN";
}

char *lcn_event_to_json(const LcnEvent *event)
{
    LcnJsonValue *obj;
    LcnJsonValue *detail;
    char *result;

    if (!event) {
        return NULL;
    }

    obj = lcn_json_object_new();

    lcn_json_set_string(obj, "kind", lcn_event_kind_name(event->kind));
    lcn_json_set_number(obj, "seq", (double)event->seq);
    lcn_json_set_number(obj, "timestamp_ms", (double)event->timestamp_ms);
    lcn_json_set_string(obj, "agent", event->agent_name);
    lcn_json_set_string(obj, "session_id", event->session_id);

    /* Try to parse detail_json; if it fails, use as raw string */
    if (event->detail_json[0] == '{' || event->detail_json[0] == '[') {
        detail = lcn_json_parse(event->detail_json,
                                 strlen(event->detail_json));
        if (detail) {
            lcn_json_set(obj, "detail", detail);
        } else {
            lcn_json_set_string(obj, "detail", event->detail_json);
        }
    } else {
        lcn_json_set_string(obj, "detail", event->detail_json);
    }

    result = lcn_json_stringify(obj);
    lcn_json_free(obj);
    return result;
}

char *lcn_events_to_json(const LcnEvent *events, int count)
{
    LcnJsonValue *arr;
    int i;
    char *result;

    arr = lcn_json_array_new();

    for (i = 0; i < count; i++) {
        LcnJsonValue *obj = lcn_json_object_new();
        LcnJsonValue *detail;

        lcn_json_set_string(obj, "kind", lcn_event_kind_name(events[i].kind));
        lcn_json_set_number(obj, "seq", (double)events[i].seq);
        lcn_json_set_number(obj, "timestamp_ms", (double)events[i].timestamp_ms);
        lcn_json_set_string(obj, "agent", events[i].agent_name);
        lcn_json_set_string(obj, "session_id", events[i].session_id);

        if (events[i].detail_json[0] == '{' || events[i].detail_json[0] == '[') {
            detail = lcn_json_parse(events[i].detail_json,
                                     strlen(events[i].detail_json));
            if (detail) {
                lcn_json_set(obj, "detail", detail);
            } else {
                lcn_json_set_string(obj, "detail", events[i].detail_json);
            }
        } else {
            lcn_json_set_string(obj, "detail", events[i].detail_json);
        }

        lcn_json_array_push(arr, obj);
    }

    result = lcn_json_stringify(arr);
    lcn_json_free(arr);
    return result;
}

uint64_t lcn_event_store_uptime_ms(const LcnEventStore *store)
{
    if (!store || !store->initialized) {
        return 0;
    }
    return now_ms() - store->start_time_ms;
}

/* ── Agent Registry ─────────────────────────────────────────────── */

void lcn_agent_registry_init(LcnAgentRegistry *reg)
{
    memset(reg, 0, sizeof(*reg));
}

LcnAgentInfo *lcn_agent_register(LcnAgentRegistry *reg,
                                    const char *name, const char *model)
{
    LcnAgentInfo *info;

    if (!reg || !name) {
        return NULL;
    }

    /* Check if already registered */
    info = lcn_agent_find(reg, name);
    if (info) {
        return info;
    }

    if (reg->count >= LCN_MAX_AGENTS) {
        return NULL;
    }

    info = &reg->agents[reg->count++];
    memset(info, 0, sizeof(*info));

    safe_copy(info->name, name, LCN_EVENT_AGENT_NAME_MAX);
    safe_copy(info->model, model, LCN_AGENT_MODEL_MAX);
    info->status = LCN_AGENT_STATUS_RUNNING;
    info->start_time_ms = now_ms();
    info->active = true;

    return info;
}

LcnAgentInfo *lcn_agent_find(const LcnAgentRegistry *reg, const char *name)
{
    int i;

    if (!reg || !name) {
        return NULL;
    }

    for (i = 0; i < reg->count; i++) {
        if (strcmp(reg->agents[i].name, name) == 0) {
            /* Cast away const — caller may need to mutate */
            return (LcnAgentInfo *)&reg->agents[i];
        }
    }

    return NULL;
}

int lcn_agent_active_count(const LcnAgentRegistry *reg)
{
    int i;
    int active = 0;

    if (!reg) {
        return 0;
    }

    for (i = 0; i < reg->count; i++) {
        if (reg->agents[i].active) {
            active++;
        }
    }

    return active;
}

char *lcn_agent_to_json(const LcnAgentInfo *agent)
{
    LcnJsonValue *obj;
    char *result;

    if (!agent) {
        return NULL;
    }

    obj = lcn_json_object_new();

    lcn_json_set_string(obj, "name", agent->name);
    lcn_json_set_string(obj, "model", agent->model);
    lcn_json_set_string(obj, "status", agent_status_string(agent->status));
    lcn_json_set_number(obj, "budget_max_tokens", (double)agent->budget_max_tokens);
    lcn_json_set_number(obj, "budget_used_tokens", (double)agent->budget_used_tokens);
    lcn_json_set_number(obj, "budget_max_cost", agent->budget_max_cost);
    lcn_json_set_number(obj, "budget_used_cost", agent->budget_used_cost);
    lcn_json_set_number(obj, "start_time_ms", (double)agent->start_time_ms);
    lcn_json_set_number(obj, "total_llm_calls", (double)agent->total_llm_calls);
    lcn_json_set_number(obj, "total_tokens", (double)agent->total_tokens);
    lcn_json_set_number(obj, "total_cost", agent->total_cost);
    lcn_json_set(obj, "active", lcn_json_bool_new(agent->active));

    result = lcn_json_stringify(obj);
    lcn_json_free(obj);
    return result;
}

char *lcn_agents_to_json(const LcnAgentRegistry *reg)
{
    LcnJsonValue *arr;
    int i;
    char *result;

    if (!reg) {
        return NULL;
    }

    arr = lcn_json_array_new();

    for (i = 0; i < reg->count; i++) {
        const LcnAgentInfo *a = &reg->agents[i];
        LcnJsonValue *obj = lcn_json_object_new();

        lcn_json_set_string(obj, "name", a->name);
        lcn_json_set_string(obj, "model", a->model);
        lcn_json_set_string(obj, "status", agent_status_string(a->status));
        lcn_json_set_number(obj, "budget_max_tokens", (double)a->budget_max_tokens);
        lcn_json_set_number(obj, "budget_used_tokens", (double)a->budget_used_tokens);
        lcn_json_set_number(obj, "budget_max_cost", a->budget_max_cost);
        lcn_json_set_number(obj, "budget_used_cost", a->budget_used_cost);
        lcn_json_set_number(obj, "start_time_ms", (double)a->start_time_ms);
        lcn_json_set_number(obj, "total_llm_calls", (double)a->total_llm_calls);
        lcn_json_set_number(obj, "total_tokens", (double)a->total_tokens);
        lcn_json_set_number(obj, "total_cost", a->total_cost);
        lcn_json_set(obj, "active", lcn_json_bool_new(a->active));

        lcn_json_array_push(arr, obj);
    }

    result = lcn_json_stringify(arr);
    lcn_json_free(arr);
    return result;
}

/* ── Convenience emit helpers (use global store) ────────────────── */

void lcn_emit_llm_request(const char *agent, const char *model,
                           const char *endpoint)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"model\":\"%s\",\"endpoint\":\"%s\"}",
             model ? model : "", endpoint ? endpoint : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_LLM_REQUEST,
                    agent, "", detail);
}

void lcn_emit_llm_response(const char *agent, int64_t tokens, double cost,
                            int64_t latency_ms)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    LcnAgentInfo *info;

    snprintf(detail, sizeof(detail),
             "{\"tokens\":%lld,\"cost\":%.6f,\"latency_ms\":%lld}",
             (long long)tokens, cost, (long long)latency_ms);
    lcn_event_emit(lcn_event_store(), LCN_EVENT_LLM_RESPONSE,
                    agent, "", detail);

    /* Update agent stats */
    info = lcn_agent_find(lcn_agent_registry(), agent);
    if (info) {
        info->total_llm_calls++;
        info->total_tokens += tokens;
        info->total_cost += cost;
    }
}

void lcn_emit_budget_deduct(const char *agent, int64_t tokens, double cost)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    LcnAgentInfo *info;

    snprintf(detail, sizeof(detail),
             "{\"tokens\":%lld,\"cost\":%.6f}",
             (long long)tokens, cost);
    lcn_event_emit(lcn_event_store(), LCN_EVENT_BUDGET_DEDUCT,
                    agent, "", detail);

    /* Update budget tracking */
    info = lcn_agent_find(lcn_agent_registry(), agent);
    if (info) {
        info->budget_used_tokens += tokens;
        info->budget_used_cost += cost;
    }
}

void lcn_emit_budget_exhausted(const char *agent, const char *reason)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    LcnAgentInfo *info;

    snprintf(detail, sizeof(detail),
             "{\"reason\":\"%s\"}", reason ? reason : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_BUDGET_EXHAUSTED,
                    agent, "", detail);

    info = lcn_agent_find(lcn_agent_registry(), agent);
    if (info) {
        info->status = LCN_AGENT_STATUS_STOPPED;
        info->active = false;
    }
}

void lcn_emit_tool_call(const char *agent, const char *tool_name,
                         const char *server)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"tool\":\"%s\",\"server\":\"%s\"}",
             tool_name ? tool_name : "", server ? server : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_TOOL_CALL,
                    agent, "", detail);
}

void lcn_emit_tool_result(const char *agent, const char *tool_name, bool ok)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"tool\":\"%s\",\"ok\":%s}",
             tool_name ? tool_name : "", ok ? "true" : "false");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_TOOL_RESULT,
                    agent, "", detail);
}

void lcn_emit_channel_send(const char *agent, const char *channel_name)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"channel\":\"%s\"}", channel_name ? channel_name : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_CHANNEL_SEND,
                    agent, "", detail);
}

void lcn_emit_channel_recv(const char *agent, const char *channel_name)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"channel\":\"%s\"}", channel_name ? channel_name : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_CHANNEL_RECV,
                    agent, "", detail);
}

void lcn_emit_agent_start(const char *agent, const char *model)
{
    char detail[LCN_EVENT_DETAIL_MAX];

    snprintf(detail, sizeof(detail),
             "{\"model\":\"%s\"}", model ? model : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_AGENT_START,
                    agent, "", detail);

    /* Auto-register the agent */
    lcn_agent_register(lcn_agent_registry(), agent, model);
}

void lcn_emit_agent_stop(const char *agent)
{
    LcnAgentInfo *info;

    lcn_event_emit(lcn_event_store(), LCN_EVENT_AGENT_STOP,
                    agent, "", "{}");

    info = lcn_agent_find(lcn_agent_registry(), agent);
    if (info) {
        info->status = LCN_AGENT_STATUS_STOPPED;
        info->active = false;
    }
}

void lcn_emit_agent_fail(const char *agent, const char *error)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    LcnAgentInfo *info;

    snprintf(detail, sizeof(detail),
             "{\"error\":\"%s\"}", error ? error : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_AGENT_FAIL,
                    agent, "", detail);

    info = lcn_agent_find(lcn_agent_registry(), agent);
    if (info) {
        info->status = LCN_AGENT_STATUS_FAILED;
        info->active = false;
    }
}

void lcn_emit_log(const char *agent, const char *message)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"message\":\"%s\"}", message ? message : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_LOG,
                    agent, "", detail);
}

void lcn_emit_memory_store(const char *agent, const char *session_id,
                            const char *key, const char *type_name)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"key\":\"%s\",\"type\":\"%s\"}",
             key ? key : "", type_name ? type_name : "");
    lcn_event_emit(lcn_event_store(), LCN_EVENT_MEMORY_STORE,
                    agent, session_id ? session_id : "", detail);
}

void lcn_emit_memory_query(const char *agent, const char *session_id,
                            int result_count)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"result_count\":%d}", result_count);
    lcn_event_emit(lcn_event_store(), LCN_EVENT_MEMORY_QUERY,
                    agent, session_id ? session_id : "", detail);
}

void lcn_emit_memory_compact(const char *agent, const char *session_id,
                              int entries_removed)
{
    char detail[LCN_EVENT_DETAIL_MAX];
    snprintf(detail, sizeof(detail),
             "{\"entries_removed\":%d}", entries_removed);
    lcn_event_emit(lcn_event_store(), LCN_EVENT_MEMORY_COMPACT,
                    agent, session_id ? session_id : "", detail);
}

/* ── Metrics ────────────────────────────────────────────────────── */

char *lcn_metrics_to_json(const LcnAgentRegistry *reg,
                           const LcnEventStore *store)
{
    LcnJsonValue *obj;
    LcnJsonValue *agents_arr;
    int i;
    int64_t sum_tokens = 0;
    int64_t sum_llm_calls = 0;
    double sum_cost = 0.0;
    int active_count = 0;
    char *result;

    obj = lcn_json_object_new();

    /* Aggregate metrics across active agents */
    if (reg) {
        for (i = 0; i < reg->count; i++) {
            if (reg->agents[i].active) {
                sum_tokens += reg->agents[i].total_tokens;
                sum_llm_calls += reg->agents[i].total_llm_calls;
                sum_cost += reg->agents[i].total_cost;
                active_count++;
            }
        }
    }

    lcn_json_set_number(obj, "active_agents", (double)active_count);
    lcn_json_set_number(obj, "total_agents", reg ? (double)reg->count : 0.0);
    lcn_json_set_number(obj, "total_tokens", (double)sum_tokens);
    lcn_json_set_number(obj, "total_llm_calls", (double)sum_llm_calls);
    lcn_json_set_number(obj, "total_cost", sum_cost);
    lcn_json_set_number(obj, "event_count",
                         store ? (double)store->count : 0.0);
    lcn_json_set_number(obj, "event_latest_seq",
                         store ? (double)lcn_event_latest_seq(store) : 0.0);
    lcn_json_set_number(obj, "uptime_ms",
                         store ? (double)lcn_event_store_uptime_ms(store)
                               : 0.0);

    /* Per-agent summary */
    agents_arr = lcn_json_array_new();
    if (reg) {
        for (i = 0; i < reg->count; i++) {
            const LcnAgentInfo *a = &reg->agents[i];
            LcnJsonValue *aobj = lcn_json_object_new();

            lcn_json_set_string(aobj, "name", a->name);
            lcn_json_set_string(aobj, "model", a->model);
            lcn_json_set_string(aobj, "status", agent_status_string(a->status));
            lcn_json_set_number(aobj, "total_llm_calls",
                                 (double)a->total_llm_calls);
            lcn_json_set_number(aobj, "total_tokens", (double)a->total_tokens);
            lcn_json_set_number(aobj, "total_cost", a->total_cost);
            lcn_json_set(aobj, "active", lcn_json_bool_new(a->active));

            lcn_json_array_push(agents_arr, aobj);
        }
    }
    lcn_json_set(obj, "agents", agents_arr);

    result = lcn_json_stringify(obj);
    lcn_json_free(obj);
    return result;
}
