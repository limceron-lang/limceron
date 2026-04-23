/*
 * Limceron Runtime — Supervisor
 *
 * Erlang/OTP-inspired supervisor for agent fault tolerance.
 * Manages agent lifecycle: start, stop, restart with strategies.
 */

#ifndef LCN_SUPERVISOR_H
#define LCN_SUPERVISOR_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Strategy enum is defined in lcn_runtime.h (LcnSupervisorStrategy) */

/* ════════════════════════════════════════════════
 * Constants
 * ════════════════════════════════════════════════ */

#define LCN_SUPERVISOR_MAX_CHILDREN  64
#define LCN_SUPERVISOR_MAX_RESTARTS  256

/* ════════════════════════════════════════════════
 * Child State
 * ════════════════════════════════════════════════ */

typedef enum {
    LCN_CHILD_STOPPED,
    LCN_CHILD_RUNNING,
    LCN_CHILD_FAILED,
    LCN_CHILD_RESTARTING
} LcnChildState;

typedef enum {
    LCN_RESTART_ALWAYS,
    LCN_RESTART_ON_FAILURE,
    LCN_RESTART_NEVER
} LcnRestartMode;

typedef struct {
    const char     *name;           /* Agent name */
    void          (*start_fn)(void);/* Function to call to start this child */
    void          (*stop_fn)(void); /* Function to call to stop this child (may be NULL) */
    LcnChildState   state;
    LcnRestartMode  restart_mode;
    int             restart_count;  /* Total restarts for this child */
    int             consecutive_failures;
} LcnSupervisorChild;

/* ════════════════════════════════════════════════
 * Restart History (sliding window)
 * ════════════════════════════════════════════════ */

typedef struct {
    time_t timestamps[LCN_SUPERVISOR_MAX_RESTARTS];
    int    head;
    int    count;
} LcnRestartHistory;

/* ════════════════════════════════════════════════
 * Supervisor
 * ════════════════════════════════════════════════ */

typedef enum {
    LCN_SUPERVISOR_STOPPED,
    LCN_SUPERVISOR_RUNNING,
    LCN_SUPERVISOR_SHUTTING_DOWN,
    LCN_SUPERVISOR_ESCALATED       /* max_restarts exceeded */
} LcnSupervisorState;

typedef struct LcnSupervisor {
    const char            *name;
    LcnSupervisorStrategy  strategy;
    int                    max_restarts;
    int                    window_seconds;
    LcnSupervisorState     state;

    /* Children */
    LcnSupervisorChild     children[LCN_SUPERVISOR_MAX_CHILDREN];
    int                    child_count;

    /* Restart tracking (sliding window) */
    LcnRestartHistory      history;

    /* Statistics */
    int                    total_restarts;
    time_t                 started_at;
    time_t                 last_restart_at;

    /* Escalation callback (optional) */
    void                 (*on_escalate)(struct LcnSupervisor *sup);
} LcnSupervisor;

/* ════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════ */

/* Create a new supervisor */
LcnSupervisor *lcn_supervisor_new(const char *name,
                                   LcnSupervisorStrategy strategy,
                                   int max_restarts,
                                   int window_seconds);

/* Add a child agent to the supervisor */
void lcn_supervisor_add_child(LcnSupervisor *sup,
                               const char *agent_name,
                               void (*start_fn)(void),
                               void (*stop_fn)(void),
                               LcnRestartMode mode);

/* Start all children in order */
void lcn_supervisor_start(LcnSupervisor *sup);

/* Stop all children in reverse order */
void lcn_supervisor_stop(LcnSupervisor *sup);

/* Report a child failure — triggers strategy logic */
int lcn_supervisor_child_failed(LcnSupervisor *sup, int child_index);

/* Query supervisor state */
bool lcn_supervisor_is_running(const LcnSupervisor *sup);
int  lcn_supervisor_restart_count(const LcnSupervisor *sup);
const char *lcn_supervisor_strategy_name(LcnSupervisorStrategy s);
const char *lcn_supervisor_state_name(LcnSupervisorState s);

/* Get child info */
int  lcn_supervisor_find_child(const LcnSupervisor *sup, const char *name);
LcnChildState lcn_supervisor_child_state(const LcnSupervisor *sup, int index);

/* Free supervisor (does NOT free child function pointers) */
void lcn_supervisor_free(LcnSupervisor *sup);

#endif /* LCN_SUPERVISOR_H */
