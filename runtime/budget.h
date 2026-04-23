/*
 * Limceron Budget Tracker
 * Tracks token usage, cost, and duration for agent budgets.
 * Pure C99, single-threaded (Stage 0).
 */
#ifndef LCN_BUDGET_H
#define LCN_BUDGET_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

typedef struct {
    /* Limits (0 = unlimited) */
    int64_t max_tokens;
    double  max_cost;
    int64_t max_duration_secs;

    /* Usage tracking */
    int64_t used_tokens;
    double  used_cost;
    time_t  start_time;

    /* State */
    bool    exhausted;
    const char *exhausted_reason;  /* which limit was hit */
} LcnBudget;

/* Initialize a budget with limits */
LcnBudget lcn_budget_new(int64_t max_tokens, double max_cost, int64_t max_duration_secs);

/* Check if budget still has capacity. Updates time-based tracking. */
bool lcn_budget_check(LcnBudget *b);

/* Deduct tokens. Returns false if budget exceeded. */
bool lcn_budget_deduct_tokens(LcnBudget *b, int64_t tokens);

/* Deduct cost. Returns false if budget exceeded. */
bool lcn_budget_deduct_cost(LcnBudget *b, double cost);

/* Get remaining capacity */
int64_t lcn_budget_remaining_tokens(const LcnBudget *b);
double  lcn_budget_remaining_cost(const LcnBudget *b);
int64_t lcn_budget_remaining_time(const LcnBudget *b);

/* Get usage summary as a string. Caller must free(). */
char *lcn_budget_summary(const LcnBudget *b);

/* Reset usage counters (keeps limits) */
void lcn_budget_reset(LcnBudget *b);

#endif /* LCN_BUDGET_H */
