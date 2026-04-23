/*
 * Limceron Runtime — Supervisor Implementation
 *
 * Erlang/OTP-inspired supervisor for agent fault tolerance.
 *
 * Strategy semantics:
 *   one_for_one:  Restart only the failed child.
 *   one_for_all:  Stop all children, then restart all children.
 *   rest_for_all: Stop the failed child and all children started after it,
 *                 then restart them in order.
 *
 * Restart window:
 *   Tracks restart timestamps in a circular buffer.  If more than
 *   max_restarts occur within window_seconds, the supervisor escalates
 *   (stops all children and transitions to ESCALATED state).
 *
 * Session contamination prevention:
 *   Each restart calls the child's start_fn with a clean slate — no
 *   crash state is carried forward, matching the Limceron spec.
 */

#include "lcn_runtime.h"
#include "supervisor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ════════════════════════════════════════════════
 * Internal helpers
 * ════════════════════════════════════════════════ */

/* Record a restart timestamp in the sliding-window history. */
static void history_record(LcnRestartHistory *h) {
    int idx = (h->head + h->count) % LCN_SUPERVISOR_MAX_RESTARTS;
    h->timestamps[idx] = time(NULL);
    if (h->count < LCN_SUPERVISOR_MAX_RESTARTS) {
        h->count++;
    } else {
        /* Buffer full — overwrite oldest; advance head */
        h->head = (h->head + 1) % LCN_SUPERVISOR_MAX_RESTARTS;
    }
}

/*
 * Count how many restarts occurred within the last `window` seconds.
 * Prunes entries outside the window from the front.
 */
static int history_count_in_window(LcnRestartHistory *h, int window_seconds) {
    if (h->count == 0 || window_seconds <= 0) return 0;

    time_t now = time(NULL);
    time_t cutoff = now - (time_t)window_seconds;
    int in_window = 0;

    for (int i = 0; i < h->count; i++) {
        int idx = (h->head + i) % LCN_SUPERVISOR_MAX_RESTARTS;
        if (h->timestamps[idx] >= cutoff) {
            in_window++;
        }
    }
    return in_window;
}

/* Start a single child (does nothing if already running or no start_fn). */
static void child_start(LcnSupervisorChild *child) {
    if (child->state == LCN_CHILD_RUNNING) return;
    if (child->start_fn) {
        child->state = LCN_CHILD_RESTARTING;
        child->start_fn();
        child->state = LCN_CHILD_RUNNING;
    }
}

/* Stop a single child (does nothing if already stopped). */
static void child_stop(LcnSupervisorChild *child) {
    if (child->state == LCN_CHILD_STOPPED) return;
    if (child->stop_fn) {
        child->stop_fn();
    }
    child->state = LCN_CHILD_STOPPED;
}

/* Restart a single child: stop then start. */
static void child_restart(LcnSupervisorChild *child) {
    child_stop(child);
    child->restart_count++;
    child_start(child);
    child->consecutive_failures = 0;
}

/* ════════════════════════════════════════════════
 * Strategy implementations
 * ════════════════════════════════════════════════ */

/*
 * one_for_one: restart only the failed child.
 */
static void strategy_one_for_one(LcnSupervisor *sup, int child_index) {
    LcnSupervisorChild *child = &sup->children[child_index];

    if (child->restart_mode == LCN_RESTART_NEVER) {
        child->state = LCN_CHILD_STOPPED;
        fprintf(stderr, "[supervisor:%s] child '%s' failed (restart_mode=never) — not restarting\n",
                sup->name, child->name);
        return;
    }

    fprintf(stderr, "[supervisor:%s] one_for_one: restarting child '%s' (restart #%d)\n",
            sup->name, child->name, child->restart_count + 1);

    child_restart(child);
}

/*
 * one_for_all: stop ALL children, then restart ALL children in order.
 */
static void strategy_one_for_all(LcnSupervisor *sup, int child_index) {
    fprintf(stderr, "[supervisor:%s] one_for_all: child '%s' failed — restarting all %d children\n",
            sup->name, sup->children[child_index].name, sup->child_count);

    /* Stop all children in reverse order */
    for (int i = sup->child_count - 1; i >= 0; i--) {
        child_stop(&sup->children[i]);
    }

    /* Restart all children in forward order */
    for (int i = 0; i < sup->child_count; i++) {
        LcnSupervisorChild *c = &sup->children[i];
        if (c->restart_mode != LCN_RESTART_NEVER) {
            c->restart_count++;
            child_start(c);
            c->consecutive_failures = 0;
        }
    }
}

/*
 * rest_for_all: stop the failed child and all children started AFTER it,
 * then restart them in order.
 */
static void strategy_rest_for_all(LcnSupervisor *sup, int child_index) {
    int count = sup->child_count - child_index;
    fprintf(stderr, "[supervisor:%s] rest_for_all: child '%s' failed — restarting %d children (index %d..%d)\n",
            sup->name, sup->children[child_index].name,
            count, child_index, sup->child_count - 1);

    /* Stop children from last down to the failed one */
    for (int i = sup->child_count - 1; i >= child_index; i--) {
        child_stop(&sup->children[i]);
    }

    /* Restart from the failed child forward */
    for (int i = child_index; i < sup->child_count; i++) {
        LcnSupervisorChild *c = &sup->children[i];
        if (c->restart_mode != LCN_RESTART_NEVER) {
            c->restart_count++;
            child_start(c);
            c->consecutive_failures = 0;
        }
    }
}

/* ════════════════════════════════════════════════
 * Escalation check
 * ════════════════════════════════════════════════ */

/*
 * Returns true if the supervisor has exceeded max_restarts within
 * the sliding window.  When this happens, the supervisor transitions
 * to ESCALATED state and stops all children.
 */
static bool check_escalation(LcnSupervisor *sup) {
    if (sup->max_restarts <= 0) return false; /* unlimited */

    int recent = history_count_in_window(&sup->history, sup->window_seconds);
    if (recent > sup->max_restarts) {
        fprintf(stderr,
                "[supervisor:%s] ESCALATION: %d restarts in %d seconds (max %d) — shutting down\n",
                sup->name, recent, sup->window_seconds, sup->max_restarts);

        sup->state = LCN_SUPERVISOR_ESCALATED;

        /* Stop all children in reverse order */
        for (int i = sup->child_count - 1; i >= 0; i--) {
            child_stop(&sup->children[i]);
        }

        /* Call escalation callback if set */
        if (sup->on_escalate) {
            sup->on_escalate(sup);
        }

        return true;
    }
    return false;
}

/* ════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════ */

LcnSupervisor *lcn_supervisor_new(const char *name,
                                   LcnSupervisorStrategy strategy,
                                   int max_restarts,
                                   int window_seconds)
{
    LcnSupervisor *sup = (LcnSupervisor *)calloc(1, sizeof(LcnSupervisor));
    if (!sup) {
        fprintf(stderr, "[supervisor] allocation failed\n");
        return NULL;
    }
    sup->name           = name;
    sup->strategy       = strategy;
    sup->max_restarts   = max_restarts;
    sup->window_seconds = window_seconds > 0 ? window_seconds : 60;
    sup->state          = LCN_SUPERVISOR_STOPPED;
    sup->child_count    = 0;
    sup->total_restarts = 0;
    sup->started_at     = 0;
    sup->last_restart_at = 0;
    sup->on_escalate    = NULL;

    /* Initialize history */
    memset(&sup->history, 0, sizeof(LcnRestartHistory));

    return sup;
}

void lcn_supervisor_add_child(LcnSupervisor *sup,
                               const char *agent_name,
                               void (*start_fn)(void),
                               void (*stop_fn)(void),
                               LcnRestartMode mode)
{
    if (!sup) return;
    if (sup->child_count >= LCN_SUPERVISOR_MAX_CHILDREN) {
        fprintf(stderr, "[supervisor:%s] cannot add child '%s' — max children (%d) reached\n",
                sup->name, agent_name, LCN_SUPERVISOR_MAX_CHILDREN);
        return;
    }

    LcnSupervisorChild *child = &sup->children[sup->child_count];
    child->name           = agent_name;
    child->start_fn       = start_fn;
    child->stop_fn        = stop_fn;
    child->state          = LCN_CHILD_STOPPED;
    child->restart_mode   = mode;
    child->restart_count  = 0;
    child->consecutive_failures = 0;

    sup->child_count++;
}

void lcn_supervisor_start(LcnSupervisor *sup) {
    if (!sup) return;
    if (sup->state == LCN_SUPERVISOR_RUNNING) return;

    fprintf(stderr, "[supervisor:%s] starting (%d children, strategy=%s, max_restarts=%d/%ds)\n",
            sup->name, sup->child_count,
            lcn_supervisor_strategy_name(sup->strategy),
            sup->max_restarts, sup->window_seconds);

    sup->state = LCN_SUPERVISOR_RUNNING;
    sup->started_at = time(NULL);

    /* Start children in order */
    for (int i = 0; i < sup->child_count; i++) {
        child_start(&sup->children[i]);
    }
}

void lcn_supervisor_stop(LcnSupervisor *sup) {
    if (!sup) return;
    if (sup->state == LCN_SUPERVISOR_STOPPED) return;

    fprintf(stderr, "[supervisor:%s] stopping all children\n", sup->name);

    sup->state = LCN_SUPERVISOR_SHUTTING_DOWN;

    /* Stop children in reverse order (last started = first stopped) */
    for (int i = sup->child_count - 1; i >= 0; i--) {
        child_stop(&sup->children[i]);
    }

    sup->state = LCN_SUPERVISOR_STOPPED;
}

int lcn_supervisor_child_failed(LcnSupervisor *sup, int child_index) {
    if (!sup) return -1;
    if (child_index < 0 || child_index >= sup->child_count) return -1;
    if (sup->state != LCN_SUPERVISOR_RUNNING) return -1;

    LcnSupervisorChild *child = &sup->children[child_index];
    child->state = LCN_CHILD_FAILED;
    child->consecutive_failures++;

    /* Record in sliding window */
    history_record(&sup->history);
    sup->total_restarts++;
    sup->last_restart_at = time(NULL);

    /* Check escalation before applying strategy */
    if (check_escalation(sup)) {
        return -2; /* escalated — supervisor shut down */
    }

    /* Apply strategy */
    switch (sup->strategy) {
        case LCN_STRATEGY_ONE_FOR_ONE:
            strategy_one_for_one(sup, child_index);
            break;
        case LCN_STRATEGY_ONE_FOR_ALL:
            strategy_one_for_all(sup, child_index);
            break;
        case LCN_STRATEGY_REST_FOR_ALL:
            strategy_rest_for_all(sup, child_index);
            break;
    }

    return 0; /* success */
}

bool lcn_supervisor_is_running(const LcnSupervisor *sup) {
    return sup && sup->state == LCN_SUPERVISOR_RUNNING;
}

int lcn_supervisor_restart_count(const LcnSupervisor *sup) {
    return sup ? sup->total_restarts : 0;
}

const char *lcn_supervisor_strategy_name(LcnSupervisorStrategy s) {
    switch (s) {
        case LCN_STRATEGY_ONE_FOR_ONE: return "one_for_one";
        case LCN_STRATEGY_REST_FOR_ALL: return "rest_for_all";
        case LCN_STRATEGY_ONE_FOR_ALL: return "one_for_all";
    }
    return "unknown";
}

const char *lcn_supervisor_state_name(LcnSupervisorState s) {
    switch (s) {
        case LCN_SUPERVISOR_STOPPED:       return "stopped";
        case LCN_SUPERVISOR_RUNNING:       return "running";
        case LCN_SUPERVISOR_SHUTTING_DOWN: return "shutting_down";
        case LCN_SUPERVISOR_ESCALATED:     return "escalated";
    }
    return "unknown";
}

int lcn_supervisor_find_child(const LcnSupervisor *sup, const char *name) {
    if (!sup || !name) return -1;
    for (int i = 0; i < sup->child_count; i++) {
        if (sup->children[i].name && strcmp(sup->children[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

LcnChildState lcn_supervisor_child_state(const LcnSupervisor *sup, int index) {
    if (!sup || index < 0 || index >= sup->child_count) return LCN_CHILD_STOPPED;
    return sup->children[index].state;
}

void lcn_supervisor_free(LcnSupervisor *sup) {
    if (sup) {
        /* Stop any running children first */
        if (sup->state == LCN_SUPERVISOR_RUNNING) {
            lcn_supervisor_stop(sup);
        }
        free(sup);
    }
}
