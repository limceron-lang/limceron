/*
 * Limceron Budget Tracker — Implementation
 * Tracks token usage, cost, and duration for agent budgets.
 * Pure C99, single-threaded (Stage 0).
 */

#include "budget.h"
#include "event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Initialization
 * ============================================================ */

LcnBudget lcn_budget_new(int64_t max_tokens, double max_cost,
                           int64_t max_duration_secs)
{
    LcnBudget b;
    memset(&b, 0, sizeof(b));
    b.max_tokens        = max_tokens;
    b.max_cost          = max_cost;
    b.max_duration_secs = max_duration_secs;
    b.start_time        = time(NULL);
    return b;
}

/* ============================================================
 * Internal helpers
 * ============================================================ */

static int64_t budget_elapsed(const LcnBudget *b)
{
    time_t now = time(NULL);
    double diff = difftime(now, b->start_time);
    return (int64_t)diff;
}

static void budget_mark_exhausted(LcnBudget *b, const char *reason)
{
    b->exhausted        = true;
    b->exhausted_reason = reason;
    lcn_emit_budget_exhausted("", reason);
}

/* ============================================================
 * Check / Deduct
 * ============================================================ */

bool lcn_budget_check(LcnBudget *b)
{
    if (!b) return false;
    if (b->exhausted) return false;

    /* Token limit */
    if (b->max_tokens > 0 && b->used_tokens >= b->max_tokens) {
        budget_mark_exhausted(b, "token limit reached");
        return false;
    }

    /* Cost limit */
    if (b->max_cost > 0.0 && b->used_cost >= b->max_cost) {
        budget_mark_exhausted(b, "cost limit reached");
        return false;
    }

    /* Duration limit */
    if (b->max_duration_secs > 0) {
        int64_t elapsed = budget_elapsed(b);
        if (elapsed >= b->max_duration_secs) {
            budget_mark_exhausted(b, "duration limit reached");
            return false;
        }
    }

    return true;
}

bool lcn_budget_deduct_tokens(LcnBudget *b, int64_t tokens)
{
    if (!b || tokens < 0) return false;
    if (b->exhausted) return false;

    /* Check token limit before deducting */
    if (b->max_tokens > 0 &&
        b->used_tokens + tokens > b->max_tokens) {
        budget_mark_exhausted(b, "token limit reached");
        return false;
    }

    /* Also check time while we are here */
    if (!lcn_budget_check(b)) return false;

    b->used_tokens += tokens;
    lcn_emit_budget_deduct("", tokens, 0.0);

    /* Re-check after deduction (may have hit exact limit) */
    if (b->max_tokens > 0 && b->used_tokens >= b->max_tokens) {
        budget_mark_exhausted(b, "token limit reached");
    }

    return true;
}

bool lcn_budget_deduct_cost(LcnBudget *b, double cost)
{
    if (!b || cost < 0.0) return false;
    if (b->exhausted) return false;

    /* Check cost limit before deducting */
    if (b->max_cost > 0.0 &&
        b->used_cost + cost > b->max_cost) {
        budget_mark_exhausted(b, "cost limit reached");
        return false;
    }

    /* Also check time while we are here */
    if (!lcn_budget_check(b)) return false;

    b->used_cost += cost;

    /* Re-check after deduction (may have hit exact limit) */
    if (b->max_cost > 0.0 && b->used_cost >= b->max_cost) {
        budget_mark_exhausted(b, "cost limit reached");
    }

    return true;
}

/* ============================================================
 * Runtime check (alias for codegen convenience)
 * ============================================================ */

bool lcn_budget_check_runtime(LcnBudget *b)
{
    return lcn_budget_check(b);
}

/* ============================================================
 * Remaining capacity
 * ============================================================ */

int64_t lcn_budget_remaining_tokens(const LcnBudget *b)
{
    if (!b || b->max_tokens <= 0) return -1;  /* unlimited */
    int64_t rem = b->max_tokens - b->used_tokens;
    return rem > 0 ? rem : 0;
}

double lcn_budget_remaining_cost(const LcnBudget *b)
{
    if (!b || b->max_cost <= 0.0) return -1.0;  /* unlimited */
    double rem = b->max_cost - b->used_cost;
    return rem > 0.0 ? rem : 0.0;
}

int64_t lcn_budget_remaining_time(const LcnBudget *b)
{
    if (!b || b->max_duration_secs <= 0) return -1;  /* unlimited */
    int64_t elapsed = budget_elapsed(b);
    int64_t rem = b->max_duration_secs - elapsed;
    return rem > 0 ? rem : 0;
}

/* ============================================================
 * Summary
 * ============================================================ */

char *lcn_budget_summary(const LcnBudget *b)
{
    if (!b) return NULL;

    int64_t elapsed = budget_elapsed(b);
    char *buf = (char *)malloc(256);
    if (!buf) return NULL;

    snprintf(buf, 256,
             "tokens: %lld/%lld, cost: $%.2f/$%.2f, time: %llds/%llds",
             (long long)b->used_tokens,
             (long long)b->max_tokens,
             b->used_cost,
             b->max_cost,
             (long long)elapsed,
             (long long)b->max_duration_secs);

    return buf;
}

/* ============================================================
 * Reset
 * ============================================================ */

void lcn_budget_reset(LcnBudget *b)
{
    if (!b) return;
    b->used_tokens      = 0;
    b->used_cost        = 0.0;
    b->start_time       = time(NULL);
    b->exhausted        = false;
    b->exhausted_reason = NULL;
}
