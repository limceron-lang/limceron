/*
 * Limceron Dashboard REST API — Implementation
 * Endpoint handlers for the agent dashboard.
 * Pure C99, single-threaded (Stage 0).
 */

#include "dashboard_api.h"
#include "httpd.h"
#include "json.h"
#include "event.h"
#include "memory.h"
#include "kb.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/select.h>

/* -------------------------------------------------------------------------- */
/*  Internal helpers                                                          */
/* -------------------------------------------------------------------------- */

static time_t server_start_time = 0;

static void ensure_start_time(void)
{
    if (server_start_time == 0) {
        server_start_time = time(NULL);
    }
}

static long uptime_ms(void)
{
    time_t now = time(NULL);
    double diff = difftime(now, server_start_time);
    return (long)(diff * 1000.0);
}

/* Parse an integer from a string, returning default_val if NULL or empty */
static long parse_long(const char *s, long default_val)
{
    char *end = NULL;
    long val;

    if (!s || *s == '\0') return default_val;

    val = strtol(s, &end, 10);
    if (end == s) return default_val;
    return val;
}

/* -------------------------------------------------------------------------- */
/*  GET /api/health                                                           */
/* -------------------------------------------------------------------------- */

void lcn_api_health(const LcnHttpRequest *req)
{
    char buf[128];
    int len;

    ensure_start_time();

    len = snprintf(buf, sizeof(buf),
                   "{\"status\":\"ok\",\"uptime_ms\":%ld}",
                   uptime_ms());
    if (len < 0) len = 0;
    lcn_httpd_respond_json(req, 200, buf);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/agents                                                           */
/* -------------------------------------------------------------------------- */

void lcn_api_agents_list(const LcnHttpRequest *req)
{
    LcnAgentRegistry *reg = lcn_agent_registry();
    char *json;

    if (!reg) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    json = lcn_agents_to_json(reg);
    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/agents/:name                                                     */
/* -------------------------------------------------------------------------- */

void lcn_api_agents_detail(const LcnHttpRequest *req)
{
    LcnAgentRegistry *reg = lcn_agent_registry();
    LcnAgentInfo *agent;
    char name[128];
    char *json;

    if (!reg) {
        lcn_httpd_respond_error(req, 404, "agent not found");
        return;
    }

    if (!lcn_httpd_path_param("/api/agents/:name", req->path,
                                "name", name, sizeof(name))) {
        lcn_httpd_respond_error(req, 400, "missing agent name");
        return;
    }

    agent = lcn_agent_find(reg, name);
    if (!agent) {
        lcn_httpd_respond_error(req, 404, "agent not found");
        return;
    }

    json = lcn_agent_to_json(agent);
    if (!json) {
        lcn_httpd_respond_error(req, 500, "serialization failed");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/events                                                           */
/* -------------------------------------------------------------------------- */

void lcn_api_events(const LcnHttpRequest *req)
{
    LcnEventStore *store = lcn_event_store();
    char param_buf[128];
    char agent_buf[128];
    long since_seq;
    long limit;
    const char *agent_filter;
    int kind_id;
    LcnEvent *events;
    int count;
    char *json;
    int k;

    if (!store) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    /* Parse query parameters */
    since_seq = 0;
    if (lcn_httpd_query_param(req, "since", param_buf, sizeof(param_buf))) {
        since_seq = parse_long(param_buf, 0);
    }

    limit = 100;
    if (lcn_httpd_query_param(req, "limit", param_buf, sizeof(param_buf))) {
        limit = parse_long(param_buf, 100);
    }
    if (limit <= 0) limit = 100;
    if (limit > 1000) limit = 1000;

    kind_id = -1;
    if (lcn_httpd_query_param(req, "kind", param_buf, sizeof(param_buf))) {
        for (k = 0; k < LCN_EVENT_KIND_COUNT; k++) {
            if (strcmp(lcn_event_kind_name((LcnEventKind)k), param_buf) == 0) {
                kind_id = k;
                break;
            }
        }
    }

    agent_filter = NULL;
    if (lcn_httpd_query_param(req, "agent", agent_buf, sizeof(agent_buf))) {
        agent_filter = agent_buf;
    }

    /* Allocate output buffer */
    events = (LcnEvent *)malloc((size_t)limit * sizeof(LcnEvent));
    if (!events) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    count = lcn_event_query(store, (uint64_t)since_seq, kind_id,
                              agent_filter, (int)limit,
                              events, (int)limit);

    json = lcn_events_to_json(events, count);
    free(events);

    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/events/stream (long-poll)                                        */
/* -------------------------------------------------------------------------- */

void lcn_api_events_stream(const LcnHttpRequest *req)
{
    LcnEventStore *store = lcn_event_store();
    char param_buf[128];
    uint64_t since_seq;
    uint64_t latest;
    struct timeval tv;
    fd_set dummy_fds;
    int polls;
    LcnEvent *events;
    int count;
    char *json;

    if (!store) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    since_seq = 0;
    if (lcn_httpd_query_param(req, "since", param_buf, sizeof(param_buf))) {
        since_seq = (uint64_t)parse_long(param_buf, 0);
    }

    /*
     * Long-poll: wait up to 5 seconds for new events.
     * Use select() with a 100ms timeout in a loop (up to 50 iterations).
     */
    for (polls = 0; polls < 50; polls++) {
        latest = lcn_event_latest_seq(store);
        if (latest > since_seq) {
            /* New events available */
            events = (LcnEvent *)malloc(100 * sizeof(LcnEvent));
            if (!events) {
                lcn_httpd_respond_json(req, 200, "[]");
                return;
            }

            count = lcn_event_query(store, since_seq, -1, NULL, 100,
                                      events, 100);
            json = lcn_events_to_json(events, count);
            free(events);

            if (json) {
                lcn_httpd_respond_json(req, 200, json);
                free(json);
                return;
            }
            lcn_httpd_respond_json(req, 200, "[]");
            return;
        }

        /* Sleep 100ms using select() on an empty fd_set */
        FD_ZERO(&dummy_fds);
        tv.tv_sec = 0;
        tv.tv_usec = 100000; /* 100ms */
        (void)select(0, &dummy_fds, NULL, NULL, &tv);
    }

    /* Timeout: return empty array */
    lcn_httpd_respond_json(req, 200, "[]");
}

/* -------------------------------------------------------------------------- */
/*  GET /api/metrics                                                          */
/* -------------------------------------------------------------------------- */

void lcn_api_metrics(const LcnHttpRequest *req)
{
    char *json = lcn_metrics_to_json(lcn_agent_registry(),
                                       lcn_event_store());

    if (!json) {
        lcn_httpd_respond_json(req, 200, "{}");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/budget                                                           */
/* -------------------------------------------------------------------------- */

void lcn_api_budget(const LcnHttpRequest *req)
{
    LcnAgentRegistry *reg = lcn_agent_registry();
    LcnJsonValue *arr;
    int i;
    char *json;

    if (!reg) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    arr = lcn_json_array_new();

    for (i = 0; i < reg->count; i++) {
        const LcnAgentInfo *a = &reg->agents[i];
        LcnJsonValue *obj = lcn_json_object_new();

        lcn_json_set_string(obj, "name", a->name);
        lcn_json_set_number(obj, "max_tokens", (double)a->budget_max_tokens);
        lcn_json_set_number(obj, "used_tokens", (double)a->budget_used_tokens);
        lcn_json_set_number(obj, "max_cost", a->budget_max_cost);
        lcn_json_set_number(obj, "used_cost", a->budget_used_cost);
        lcn_json_set(obj, "active", lcn_json_bool_new(a->active));

        lcn_json_array_push(arr, obj);
    }

    json = lcn_json_stringify(arr);
    lcn_json_free(arr);

    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/guards                                                           */
/* -------------------------------------------------------------------------- */

void lcn_api_guards(const LcnHttpRequest *req)
{
    LcnEventStore *store = lcn_event_store();
    LcnEvent *events;
    LcnJsonValue *arr;
    int count_check;
    int count_trigger;
    char *json;
    int total;
    int i;

    if (!store) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    /* Query both GUARD_CHECK and GUARD_TRIGGER events */
    events = (LcnEvent *)malloc(400 * sizeof(LcnEvent));
    if (!events) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    count_check = lcn_event_query(store, 0,
                                    (int)LCN_EVENT_GUARD_CHECK,
                                    NULL, 200, events, 200);
    count_trigger = lcn_event_query(store, 0,
                                      (int)LCN_EVENT_GUARD_TRIGGER,
                                      NULL, 200,
                                      events + count_check, 200);
    total = count_check + count_trigger;

    /* Build JSON array from matched events */
    arr = lcn_json_array_new();
    for (i = 0; i < total; i++) {
        LcnJsonValue *detail;
        LcnJsonValue *obj = lcn_json_object_new();

        lcn_json_set_string(obj, "kind",
                              lcn_event_kind_name(events[i].kind));
        lcn_json_set_number(obj, "seq", (double)events[i].seq);
        lcn_json_set_number(obj, "timestamp_ms",
                              (double)events[i].timestamp_ms);
        lcn_json_set_string(obj, "agent", events[i].agent_name);

        if (events[i].detail_json[0] == '{') {
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

    free(events);

    json = lcn_json_stringify(arr);
    lcn_json_free(arr);

    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/capabilities                                                     */
/*                                                                            */
/*  Returns a structured view of all agents, their capabilities, and any      */
/*  active delegations.  Format:                                              */
/*  {                                                                         */
/*    "agents": [                                                             */
/*      {                                                                     */
/*        "name": "Categorizer",                                              */
/*        "capabilities": ["llm.gpt4", "data.read"],                          */
/*        "delegated_to": [                                                   */
/*          {"agent": "SubAgent", "caps": ["data.read"],                      */
/*           "expires_ms": 30000}                                             */
/*        ]                                                                   */
/*      }                                                                     */
/*    ]                                                                       */
/*  }                                                                         */
/* -------------------------------------------------------------------------- */

/* Max number of capability names we can extract from events per agent */
#define LCN_CAP_VIEW_MAX_CAPS      64
#define LCN_CAP_VIEW_MAX_DELEG     32
#define LCN_CAP_VIEW_NAME_MAX     128

/* A single delegation entry for the JSON response */
typedef struct {
    char agent[LCN_CAP_VIEW_NAME_MAX];
    char caps[LCN_CAP_VIEW_MAX_CAPS][LCN_CAP_VIEW_NAME_MAX];
    int  cap_count;
    long expires_ms;
} LcnCapDelegView;

/* Per-agent capability view */
typedef struct {
    char              name[LCN_CAP_VIEW_NAME_MAX];
    char              caps[LCN_CAP_VIEW_MAX_CAPS][LCN_CAP_VIEW_NAME_MAX];
    int               cap_count;
    LcnCapDelegView   delegations[LCN_CAP_VIEW_MAX_DELEG];
    int               deleg_count;
} LcnCapAgentView;

/* Helper: check if a capability string is already present in an array */
static bool cap_view_has_cap(char caps[][LCN_CAP_VIEW_NAME_MAX],
                              int count, const char *name)
{
    int i;
    for (i = 0; i < count; i++) {
        if (strcmp(caps[i], name) == 0) return true;
    }
    return false;
}

/* Helper: find or create an agent view by name */
static LcnCapAgentView *cap_view_find_agent(LcnCapAgentView *views,
                                              int *count, int max,
                                              const char *name)
{
    int i;
    for (i = 0; i < *count; i++) {
        if (strcmp(views[i].name, name) == 0) return &views[i];
    }
    if (*count >= max) return NULL;
    {
        LcnCapAgentView *v = &views[*count];
        memset(v, 0, sizeof(*v));
        snprintf(v->name, sizeof(v->name), "%s", name);
        (*count)++;
        return v;
    }
}

/* Helper: add a capability name to an agent view (deduplicating) */
static void cap_view_add_cap(LcnCapAgentView *v, const char *cap_name)
{
    if (!v || !cap_name || cap_name[0] == '\0') return;
    if (v->cap_count >= LCN_CAP_VIEW_MAX_CAPS) return;
    if (cap_view_has_cap(v->caps, v->cap_count, cap_name)) return;
    snprintf(v->caps[v->cap_count], LCN_CAP_VIEW_NAME_MAX, "%s", cap_name);
    v->cap_count++;
}

/* Helper: add a delegation entry to an agent view */
static void cap_view_add_delegation(LcnCapAgentView *v,
                                     const char *child_agent,
                                     const char *cap_name,
                                     long expires_ms)
{
    int i;
    LcnCapDelegView *d;

    if (!v || !child_agent) return;
    if (v->deleg_count >= LCN_CAP_VIEW_MAX_DELEG) return;

    /* Try to merge into existing delegation to the same child agent */
    for (i = 0; i < v->deleg_count; i++) {
        d = &v->delegations[i];
        if (strcmp(d->agent, child_agent) == 0) {
            if (cap_name && d->cap_count < LCN_CAP_VIEW_MAX_CAPS) {
                if (!cap_view_has_cap(d->caps, d->cap_count, cap_name)) {
                    snprintf(d->caps[d->cap_count], LCN_CAP_VIEW_NAME_MAX,
                             "%s", cap_name);
                    d->cap_count++;
                }
            }
            if (expires_ms > 0 && (d->expires_ms == 0 ||
                                     expires_ms < d->expires_ms)) {
                d->expires_ms = expires_ms;
            }
            return;
        }
    }

    /* New delegation entry */
    d = &v->delegations[v->deleg_count];
    memset(d, 0, sizeof(*d));
    snprintf(d->agent, sizeof(d->agent), "%s", child_agent);
    if (cap_name) {
        snprintf(d->caps[0], LCN_CAP_VIEW_NAME_MAX, "%s", cap_name);
        d->cap_count = 1;
    }
    d->expires_ms = expires_ms;
    v->deleg_count++;
}

void lcn_api_capabilities(const LcnHttpRequest *req)
{
    LcnAgentRegistry *reg = lcn_agent_registry();
    LcnEventStore *store = lcn_event_store();
    LcnCapAgentView views[LCN_MAX_AGENTS];
    int view_count = 0;
    LcnJsonValue *root;
    LcnJsonValue *agents_arr;
    char *json;
    int i;

    memset(views, 0, sizeof(views));

    /* Step 1: seed from agent registry (every registered agent appears) */
    if (reg) {
        for (i = 0; i < reg->count; i++) {
            cap_view_find_agent(views, &view_count, LCN_MAX_AGENTS,
                                  reg->agents[i].name);
        }
    }

    /* Step 2: enrich from CAPABILITY_ACCESS events.
     * Each event's detail_json may contain:
     *   {"cap":"llm.gpt4","agent":"Bot"}
     *   {"cap":"data.read","delegate_to":"Sub","expires_ms":30000}
     */
    if (store) {
        LcnEvent *events;
        int ev_count;

        events = (LcnEvent *)malloc(500 * sizeof(LcnEvent));
        if (events) {
            ev_count = lcn_event_query(store, 0,
                                         (int)LCN_EVENT_CAPABILITY_ACCESS,
                                         NULL, 500, events, 500);
            for (i = 0; i < ev_count; i++) {
                const LcnEvent *ev = &events[i];
                LcnCapAgentView *v;
                LcnJsonValue *detail;
                const char *cap_name;
                const char *deleg_to;
                double exp_ms;

                v = cap_view_find_agent(views, &view_count,
                                          LCN_MAX_AGENTS,
                                          ev->agent_name);
                if (!v) continue;

                /* Try to parse structured detail */
                if (ev->detail_json[0] == '{') {
                    detail = lcn_json_parse(ev->detail_json,
                                              strlen(ev->detail_json));
                    if (detail) {
                        cap_name = lcn_json_get_string(detail, "cap");
                        deleg_to = lcn_json_get_string(detail,
                                                         "delegate_to");
                        exp_ms = lcn_json_get_number(detail, "expires_ms");

                        if (cap_name) {
                            cap_view_add_cap(v, cap_name);
                        }
                        if (deleg_to) {
                            cap_view_add_delegation(v, deleg_to,
                                                      cap_name,
                                                      (long)exp_ms);
                        }
                        lcn_json_free(detail);
                    }
                } else if (ev->detail_json[0] != '\0') {
                    /* Plain string — treat as a capability name */
                    cap_view_add_cap(v, ev->detail_json);
                }
            }
            free(events);
        }
    }

    /* Step 3: build JSON response */
    root = lcn_json_object_new();
    agents_arr = lcn_json_array_new();

    for (i = 0; i < view_count; i++) {
        LcnCapAgentView *v = &views[i];
        LcnJsonValue *agent_obj = lcn_json_object_new();
        LcnJsonValue *caps_arr = lcn_json_array_new();
        LcnJsonValue *deleg_arr = lcn_json_array_new();
        int j;

        lcn_json_set_string(agent_obj, "name", v->name);

        for (j = 0; j < v->cap_count; j++) {
            lcn_json_array_push(caps_arr,
                                  lcn_json_string_new(v->caps[j]));
        }
        lcn_json_set(agent_obj, "capabilities", caps_arr);

        for (j = 0; j < v->deleg_count; j++) {
            LcnCapDelegView *d = &v->delegations[j];
            LcnJsonValue *d_obj = lcn_json_object_new();
            LcnJsonValue *d_caps = lcn_json_array_new();
            int k;

            lcn_json_set_string(d_obj, "agent", d->agent);
            for (k = 0; k < d->cap_count; k++) {
                lcn_json_array_push(d_caps,
                                      lcn_json_string_new(d->caps[k]));
            }
            lcn_json_set(d_obj, "caps", d_caps);
            lcn_json_set_number(d_obj, "expires_ms",
                                  (double)d->expires_ms);
            lcn_json_array_push(deleg_arr, d_obj);
        }
        lcn_json_set(agent_obj, "delegated_to", deleg_arr);

        lcn_json_array_push(agents_arr, agent_obj);
    }

    lcn_json_set(root, "agents", agents_arr);

    json = lcn_json_stringify(root);
    lcn_json_free(root);

    if (!json) {
        lcn_httpd_respond_json(req, 200, "{\"agents\":[]}");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  POST /api/agents/:name/pause                                              */
/* -------------------------------------------------------------------------- */

void lcn_api_agent_pause(const LcnHttpRequest *req)
{
    LcnAgentRegistry *reg = lcn_agent_registry();
    LcnAgentInfo *agent;
    char name[128];

    if (!reg) {
        lcn_httpd_respond_error(req, 404, "agent not found");
        return;
    }

    if (!lcn_httpd_path_param("/api/agents/:name/pause", req->path,
                                "name", name, sizeof(name))) {
        lcn_httpd_respond_error(req, 400, "missing agent name");
        return;
    }

    agent = lcn_agent_find(reg, name);
    if (!agent) {
        lcn_httpd_respond_error(req, 404, "agent not found");
        return;
    }

    agent->status = LCN_AGENT_STATUS_PAUSED;
    lcn_httpd_respond_json(req, 200, "{\"ok\":true}");
}

/* -------------------------------------------------------------------------- */
/*  POST /api/agents/:name/resume                                             */
/* -------------------------------------------------------------------------- */

void lcn_api_agent_resume(const LcnHttpRequest *req)
{
    LcnAgentRegistry *reg = lcn_agent_registry();
    LcnAgentInfo *agent;
    char name[128];

    if (!reg) {
        lcn_httpd_respond_error(req, 404, "agent not found");
        return;
    }

    if (!lcn_httpd_path_param("/api/agents/:name/resume", req->path,
                                "name", name, sizeof(name))) {
        lcn_httpd_respond_error(req, 400, "missing agent name");
        return;
    }

    agent = lcn_agent_find(reg, name);
    if (!agent) {
        lcn_httpd_respond_error(req, 404, "agent not found");
        return;
    }

    agent->status = LCN_AGENT_STATUS_RUNNING;
    lcn_httpd_respond_json(req, 200, "{\"ok\":true}");
}

/* -------------------------------------------------------------------------- */
/*  GET /api/memory                                                           */
/* -------------------------------------------------------------------------- */

void lcn_api_memory_list(const LcnHttpRequest *req)
{
    LcnMemoryStore *store = lcn_memory_store();
    char agent_buf[128];
    char session_buf[128];
    char type_buf[64];
    char limit_buf[32];
    LcnMemoryEntry *entries;
    int count = 0;
    int limit;
    char *json;

    if (!store || !store->initialized) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    limit = 100;
    if (lcn_httpd_query_param(req, "limit", limit_buf, sizeof(limit_buf))) {
        limit = (int)parse_long(limit_buf, 100);
    }
    if (limit <= 0) limit = 100;
    if (limit > 500) limit = 500;

    entries = (LcnMemoryEntry *)malloc((size_t)limit * sizeof(LcnMemoryEntry));
    if (!entries) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    if (lcn_httpd_query_param(req, "agent", agent_buf, sizeof(agent_buf))) {
        if (lcn_httpd_query_param(req, "session", session_buf, sizeof(session_buf))) {
            count = lcn_memory_query_session(store, agent_buf, session_buf,
                                              limit, entries, limit);
        } else if (lcn_httpd_query_param(req, "type", type_buf, sizeof(type_buf))) {
            int t;
            LcnMemoryType mem_type = LCN_MEM_MESSAGE;
            for (t = 0; t < LCN_MEM_TYPE_COUNT; t++) {
                if (strcmp(lcn_memory_type_name((LcnMemoryType)t), type_buf) == 0) {
                    mem_type = (LcnMemoryType)t;
                    break;
                }
            }
            count = lcn_memory_query_type(store, agent_buf, mem_type,
                                           limit, entries, limit);
        } else {
            count = lcn_memory_query_session(store, agent_buf, "",
                                              limit, entries, limit);
        }
    }

    json = lcn_memory_entries_to_json(entries, count);

    {
        int i;
        for (i = 0; i < count; i++) lcn_memory_entry_free(&entries[i]);
    }
    free(entries);

    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/memory/search                                                    */
/* -------------------------------------------------------------------------- */

void lcn_api_memory_search(const LcnHttpRequest *req)
{
    LcnMemoryStore *store = lcn_memory_store();
    char key_buf[256];
    char agent_buf[128];
    char limit_buf[32];
    int limit;
    int count;
    LcnMemoryEntry *entries;
    char *json;
    const char *agent_filter;

    if (!store || !store->initialized) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    if (!lcn_httpd_query_param(req, "key", key_buf, sizeof(key_buf))) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    limit = 50;
    if (lcn_httpd_query_param(req, "limit", limit_buf, sizeof(limit_buf))) {
        limit = (int)parse_long(limit_buf, 50);
    }
    if (limit <= 0) limit = 50;
    if (limit > 200) limit = 200;

    entries = (LcnMemoryEntry *)malloc((size_t)limit * sizeof(LcnMemoryEntry));
    if (!entries) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    agent_filter = NULL;
    if (lcn_httpd_query_param(req, "agent", agent_buf, sizeof(agent_buf))) {
        agent_filter = agent_buf;
    }

    count = lcn_memory_query_key(store, agent_filter, key_buf,
                                  limit, entries, limit);

    json = lcn_memory_entries_to_json(entries, count);
    {
        int i;
        for (i = 0; i < count; i++) lcn_memory_entry_free(&entries[i]);
    }
    free(entries);

    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }
    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/memory/sessions                                                  */
/* -------------------------------------------------------------------------- */

void lcn_api_memory_sessions(const LcnHttpRequest *req)
{
    LcnMemoryStore *store = lcn_memory_store();
    char agent_buf[128];
    LcnMemorySession sessions[100];
    int count;
    char *json;

    if (!store || !store->initialized) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    if (!lcn_httpd_query_param(req, "agent", agent_buf, sizeof(agent_buf))) {
        agent_buf[0] = '\0';
    }

    count = lcn_session_list(store, agent_buf[0] ? agent_buf : "",
                              sessions, 100);

    json = lcn_memory_sessions_to_json(sessions, count);

    {
        int i;
        for (i = 0; i < count; i++) lcn_memory_session_free(&sessions[i]);
    }

    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }
    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  POST /api/memory                                                          */
/* -------------------------------------------------------------------------- */

void lcn_api_memory_store_endpoint(const LcnHttpRequest *req)
{
    LcnMemoryStore *store = lcn_memory_store();
    LcnJsonValue *body;
    const char *agent;
    const char *session_id;
    const char *type_str;
    const char *key;
    const char *value;
    LcnMemoryType type;
    int64_t ttl_ms;
    int64_t id;
    char result[128];

    if (!store || !store->initialized) {
        lcn_httpd_respond_error(req, 500, "memory not initialized");
        return;
    }

    if (req->body_len == 0) {
        lcn_httpd_respond_error(req, 400, "empty body");
        return;
    }

    body = lcn_json_parse(req->body, req->body_len);
    if (!body) {
        lcn_httpd_respond_error(req, 400, "invalid JSON");
        return;
    }

    agent = lcn_json_get_string(body, "agent");
    session_id = lcn_json_get_string(body, "session_id");
    type_str = lcn_json_get_string(body, "type");
    key = lcn_json_get_string(body, "key");
    value = lcn_json_get_string(body, "value");
    ttl_ms = (int64_t)lcn_json_get_number(body, "ttl_ms");

    if (!agent || !value) {
        lcn_json_free(body);
        lcn_httpd_respond_error(req, 400, "agent and value required");
        return;
    }

    type = LCN_MEM_MESSAGE;
    if (type_str) {
        int t;
        for (t = 0; t < LCN_MEM_TYPE_COUNT; t++) {
            if (strcmp(lcn_memory_type_name((LcnMemoryType)t), type_str) == 0) {
                type = (LcnMemoryType)t;
                break;
            }
        }
    }

    /* Wrap value as JSON string if not already JSON */
    {
        char *value_json;
        if (value[0] == '{' || value[0] == '[' || value[0] == '"') {
            value_json = strdup(value);
        } else {
            size_t vlen = strlen(value);
            value_json = (char *)malloc(vlen + 3);
            if (value_json) {
                snprintf(value_json, vlen + 3, "\"%s\"", value);
            }
        }

        id = lcn_memory_insert(store, agent, session_id, type, key,
                                value_json ? value_json : "\"\"",
                                NULL, 0, ttl_ms);
        free(value_json);
    }

    lcn_json_free(body);

    if (id < 0) {
        lcn_httpd_respond_error(req, 500, "insert failed");
        return;
    }

    snprintf(result, sizeof(result), "{\"ok\":true,\"id\":%lld}", (long long)id);
    lcn_httpd_respond_json(req, 201, result);
}

/* -------------------------------------------------------------------------- */
/*  DELETE /api/memory/:id                                                    */
/* -------------------------------------------------------------------------- */

void lcn_api_memory_delete(const LcnHttpRequest *req)
{
    LcnMemoryStore *store = lcn_memory_store();
    char id_buf[64];
    int64_t id;
    char *end;

    if (!store || !store->initialized) {
        lcn_httpd_respond_error(req, 500, "memory not initialized");
        return;
    }

    if (!lcn_httpd_path_param("/api/memory/:id", req->path,
                                "id", id_buf, sizeof(id_buf))) {
        lcn_httpd_respond_error(req, 400, "missing id");
        return;
    }

    id = strtoll(id_buf, &end, 10);
    if (end == id_buf) {
        lcn_httpd_respond_error(req, 400, "invalid id");
        return;
    }

    if (lcn_memory_delete(store, id)) {
        lcn_httpd_respond_json(req, 200, "{\"ok\":true}");
    } else {
        lcn_httpd_respond_error(req, 404, "entry not found");
    }
}

/* -------------------------------------------------------------------------- */
/*  GET /api/kb/search                                                        */
/* -------------------------------------------------------------------------- */

void lcn_api_kb_search(const LcnHttpRequest *req)
{
    LcnKnowledgeBase *kb = lcn_kb_store();
    char query_buf[512];
    char limit_buf[32];
    int limit;
    LcnKbChunk *chunks;
    int count;
    char *json;

    if (!kb || !kb->initialized) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    if (!lcn_httpd_query_param(req, "q", query_buf, sizeof(query_buf))) {
        lcn_httpd_respond_error(req, 400, "missing ?q= parameter");
        return;
    }

    limit = 10;
    if (lcn_httpd_query_param(req, "limit", limit_buf, sizeof(limit_buf))) {
        limit = (int)parse_long(limit_buf, 10);
    }
    if (limit <= 0) limit = 10;
    if (limit > 50) limit = 50;

    chunks = (LcnKbChunk *)malloc((size_t)limit * sizeof(LcnKbChunk));
    if (!chunks) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }

    count = lcn_kb_search(kb, query_buf, limit, chunks, limit);

    json = lcn_kb_chunks_to_json(chunks, count);
    {
        int i;
        for (i = 0; i < count; i++) lcn_kb_chunk_free(&chunks[i]);
    }
    free(chunks);

    if (!json) {
        lcn_httpd_respond_json(req, 200, "[]");
        return;
    }
    lcn_httpd_respond_json(req, 200, json);
    free(json);
}

/* -------------------------------------------------------------------------- */
/*  GET /api/kb/status                                                        */
/* -------------------------------------------------------------------------- */

void lcn_api_kb_status(const LcnHttpRequest *req)
{
    LcnKnowledgeBase *kb = lcn_kb_store();
    char buf[256];

    if (!kb || !kb->initialized) {
        lcn_httpd_respond_json(req, 200,
            "{\"initialized\":false,\"chunks\":0}");
        return;
    }

    snprintf(buf, sizeof(buf),
             "{\"initialized\":true,\"ingested\":%s,"
             "\"chunks\":%d,\"chunk_size\":%d,\"chunk_overlap\":%d,"
             "\"db_path\":\"%s\"}",
             kb->ingested ? "true" : "false",
             lcn_kb_chunk_count(kb),
             kb->chunk_size, kb->chunk_overlap,
             kb->db_path);
    lcn_httpd_respond_json(req, 200, buf);
}

/* -------------------------------------------------------------------------- */
/*  Route registration                                                        */
/* -------------------------------------------------------------------------- */

void lcn_dashboard_register_routes(LcnHttpServer *server)
{
    ensure_start_time();

    lcn_httpd_route(server, "GET",  "/api/health",
                     lcn_api_health);
    lcn_httpd_route(server, "GET",  "/api/agents",
                     lcn_api_agents_list);
    lcn_httpd_route(server, "GET",  "/api/agents/:name",
                     lcn_api_agents_detail);
    lcn_httpd_route(server, "GET",  "/api/events",
                     lcn_api_events);
    lcn_httpd_route(server, "GET",  "/api/events/stream",
                     lcn_api_events_stream);
    lcn_httpd_route(server, "GET",  "/api/metrics",
                     lcn_api_metrics);
    lcn_httpd_route(server, "GET",  "/api/budget",
                     lcn_api_budget);
    lcn_httpd_route(server, "GET",  "/api/guards",
                     lcn_api_guards);
    lcn_httpd_route(server, "GET",  "/api/capabilities",
                     lcn_api_capabilities);
    lcn_httpd_route(server, "POST", "/api/agents/:name/pause",
                     lcn_api_agent_pause);
    lcn_httpd_route(server, "POST", "/api/agents/:name/resume",
                     lcn_api_agent_resume);

    /* Memory endpoints */
    lcn_httpd_route(server, "GET",    "/api/memory",
                     lcn_api_memory_list);
    lcn_httpd_route(server, "GET",    "/api/memory/search",
                     lcn_api_memory_search);
    lcn_httpd_route(server, "GET",    "/api/memory/sessions",
                     lcn_api_memory_sessions);
    lcn_httpd_route(server, "POST",   "/api/memory",
                     lcn_api_memory_store_endpoint);
    lcn_httpd_route(server, "DELETE", "/api/memory/:id",
                     lcn_api_memory_delete);

    /* Knowledge base endpoints */
    lcn_httpd_route(server, "GET",    "/api/kb/search",
                     lcn_api_kb_search);
    lcn_httpd_route(server, "GET",    "/api/kb/status",
                     lcn_api_kb_status);
}
