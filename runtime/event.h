/*
 * Limceron Event Store — Runtime Instrumentation
 * Ring buffer event store for agent observability.
 * C99, single-threaded (Stage 0).
 */
#ifndef LCN_EVENT_H
#define LCN_EVENT_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef enum {
    LCN_EVENT_AGENT_START,
    LCN_EVENT_AGENT_STOP,
    LCN_EVENT_AGENT_FAIL,
    LCN_EVENT_LLM_REQUEST,
    LCN_EVENT_LLM_RESPONSE,
    LCN_EVENT_BUDGET_DEDUCT,
    LCN_EVENT_BUDGET_EXHAUSTED,
    LCN_EVENT_GUARD_CHECK,
    LCN_EVENT_GUARD_TRIGGER,
    LCN_EVENT_CAPABILITY_ACCESS,
    LCN_EVENT_TOOL_CALL,
    LCN_EVENT_TOOL_RESULT,
    LCN_EVENT_CHANNEL_SEND,
    LCN_EVENT_CHANNEL_RECV,
    LCN_EVENT_LOG,
    LCN_EVENT_MEMORY_STORE,
    LCN_EVENT_MEMORY_QUERY,
    LCN_EVENT_MEMORY_COMPACT,
    LCN_EVENT_KIND_COUNT
} LcnEventKind;

#define LCN_EVENT_AGENT_NAME_MAX  64
#define LCN_EVENT_SESSION_ID_MAX  64
#define LCN_EVENT_DETAIL_MAX      512
#define LCN_EVENT_STORE_CAPACITY  8192
#define LCN_AGENT_MODEL_MAX       64
#define LCN_MAX_AGENTS            64

typedef struct {
    LcnEventKind kind;
    uint64_t      timestamp_ms;
    uint64_t      seq;
    char          agent_name[LCN_EVENT_AGENT_NAME_MAX];
    char          session_id[LCN_EVENT_SESSION_ID_MAX];
    char          detail_json[LCN_EVENT_DETAIL_MAX];
} LcnEvent;

typedef struct {
    LcnEvent events[LCN_EVENT_STORE_CAPACITY];
    uint64_t  head;
    uint64_t  count;
    uint64_t  next_seq;
    uint64_t  start_time_ms;
    bool      initialized;
} LcnEventStore;

typedef enum {
    LCN_AGENT_STATUS_RUNNING,
    LCN_AGENT_STATUS_PAUSED,
    LCN_AGENT_STATUS_STOPPED,
    LCN_AGENT_STATUS_FAILED
} LcnAgentStatus;

typedef struct {
    char            name[LCN_EVENT_AGENT_NAME_MAX];
    char            model[LCN_AGENT_MODEL_MAX];
    LcnAgentStatus status;
    int64_t         budget_max_tokens;
    int64_t         budget_used_tokens;
    double          budget_max_cost;
    double          budget_used_cost;
    uint64_t        start_time_ms;
    int64_t         total_llm_calls;
    int64_t         total_tokens;
    double          total_cost;
    bool            active;
} LcnAgentInfo;

typedef struct {
    LcnAgentInfo agents[LCN_MAX_AGENTS];
    int           count;
} LcnAgentRegistry;

/* Global accessors */
LcnEventStore    *lcn_event_store(void);
LcnAgentRegistry *lcn_agent_registry(void);

/* Event Store API */
void lcn_event_store_init(LcnEventStore *store);
uint64_t lcn_event_emit(LcnEventStore *store, LcnEventKind kind,
                          const char *agent_name, const char *session_id,
                          const char *detail_json);
int lcn_event_query(const LcnEventStore *store,
                     uint64_t since_seq, int filter_kind,
                     const char *filter_agent, int limit,
                     LcnEvent *out_events, int out_cap);
uint64_t lcn_event_latest_seq(const LcnEventStore *store);
const char *lcn_event_kind_name(LcnEventKind kind);
char *lcn_event_to_json(const LcnEvent *event);
char *lcn_events_to_json(const LcnEvent *events, int count);
uint64_t lcn_event_store_uptime_ms(const LcnEventStore *store);

/* Agent Registry API */
void lcn_agent_registry_init(LcnAgentRegistry *reg);
LcnAgentInfo *lcn_agent_register(LcnAgentRegistry *reg,
                                    const char *name, const char *model);
LcnAgentInfo *lcn_agent_find(const LcnAgentRegistry *reg, const char *name);
int lcn_agent_active_count(const LcnAgentRegistry *reg);
char *lcn_agent_to_json(const LcnAgentInfo *agent);
char *lcn_agents_to_json(const LcnAgentRegistry *reg);

/* Convenience emit helpers (use global store) */
void lcn_emit_llm_request(const char *agent, const char *model, const char *endpoint);
void lcn_emit_llm_response(const char *agent, int64_t tokens, double cost, int64_t latency_ms);
void lcn_emit_budget_deduct(const char *agent, int64_t tokens, double cost);
void lcn_emit_budget_exhausted(const char *agent, const char *reason);
void lcn_emit_tool_call(const char *agent, const char *tool_name, const char *server);
void lcn_emit_tool_result(const char *agent, const char *tool_name, bool ok);
void lcn_emit_channel_send(const char *agent, const char *channel_name);
void lcn_emit_channel_recv(const char *agent, const char *channel_name);
void lcn_emit_agent_start(const char *agent, const char *model);
void lcn_emit_agent_stop(const char *agent);
void lcn_emit_agent_fail(const char *agent, const char *error);
void lcn_emit_log(const char *agent, const char *message);
void lcn_emit_memory_store(const char *agent, const char *session_id, const char *key, const char *type_name);
void lcn_emit_memory_query(const char *agent, const char *session_id, int result_count);
void lcn_emit_memory_compact(const char *agent, const char *session_id, int entries_removed);

/* Metrics */
char *lcn_metrics_to_json(const LcnAgentRegistry *reg, const LcnEventStore *store);

#endif /* LCN_EVENT_H */
