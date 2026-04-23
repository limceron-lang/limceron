/*
 * Limceron Runtime — Inference Router with Health Checking
 *
 * Implements model/endpoint selection with health monitoring,
 * latency tracking, cost-aware routing, and automatic failover.
 *
 * Design:
 *   - Each router manages up to 32 endpoints
 *   - Health checks are HTTP GETs to endpoint URLs
 *   - Selection strategies: latency, cost, round-robin, failover
 *   - Automatic unhealthy marking after N consecutive failures
 *   - Callbacks for health state transitions
 */

#include "router.h"
#include "http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

/* ════════════════════════════════════════════════
 * Time Utilities
 * ════════════════════════════════════════════════ */

int64_t lcn_router_now_ms(void)
{
#ifdef __APPLE__
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    return (int64_t)((t * tb.numer / tb.denom) / 1000000ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
#endif
}

/* ════════════════════════════════════════════════
 * Constructor / Destructor
 * ════════════════════════════════════════════════ */

LcnRouter *lcn_router_new(const char *name)
{
    LcnRouter *r = (LcnRouter *)calloc(1, sizeof(LcnRouter));
    if (!r) return NULL;

    if (name) {
        size_t len = strlen(name);
        if (len >= LCN_ROUTER_MAX_NAME) len = LCN_ROUTER_MAX_NAME - 1;
        memcpy(r->name, name, len);
        r->name[len] = '\0';
    }

    r->endpoint_count = 0;
    r->health_check_interval_ms = 30000; /* 30 seconds default */
    r->strategy = LCN_ROUTE_STRATEGY_LATENCY;
    r->round_robin_idx = 0;
    r->max_consecutive_failures = 3;
    r->on_unhealthy = NULL;
    r->on_recovery = NULL;

    return r;
}

void lcn_router_free(LcnRouter *r)
{
    if (!r) return;
    free(r);
}

/* ════════════════════════════════════════════════
 * Endpoint Management
 * ════════════════════════════════════════════════ */

int lcn_router_add_endpoint(LcnRouter *r, const char *name, const char *url,
                             double cost_per_token)
{
    if (!r || !name || !url) return -1;
    if (r->endpoint_count >= LCN_ROUTER_MAX_ENDPOINTS) return -1;

    LcnRouterEndpoint *ep = &r->endpoints[r->endpoint_count];
    memset(ep, 0, sizeof(LcnRouterEndpoint));

    {
        size_t len = strlen(name);
        if (len >= LCN_ROUTER_MAX_NAME) len = LCN_ROUTER_MAX_NAME - 1;
        memcpy(ep->name, name, len);
        ep->name[len] = '\0';
    }
    {
        size_t len = strlen(url);
        if (len >= sizeof(ep->endpoint)) len = sizeof(ep->endpoint) - 1;
        memcpy(ep->endpoint, url, len);
        ep->endpoint[len] = '\0';
    }

    ep->health_status = LCN_HEALTH_UNKNOWN;
    ep->last_check_ms = 0;
    ep->avg_latency_ms = 0;
    ep->cost_per_token = cost_per_token;
    ep->total_calls = 0;
    ep->total_failures = 0;
    ep->consecutive_failures = 0;

    return r->endpoint_count++;
}

LcnRouterEndpoint *lcn_router_get_endpoint(LcnRouter *r, int idx)
{
    if (!r || idx < 0 || idx >= r->endpoint_count) return NULL;
    return &r->endpoints[idx];
}

LcnRouterEndpoint *lcn_router_find_endpoint(LcnRouter *r, const char *name)
{
    int i;
    if (!r || !name) return NULL;
    for (i = 0; i < r->endpoint_count; i++) {
        if (strcmp(r->endpoints[i].name, name) == 0) {
            return &r->endpoints[i];
        }
    }
    return NULL;
}

/* ════════════════════════════════════════════════
 * Health Checking
 * ════════════════════════════════════════════════ */

bool lcn_router_health_check_due(LcnRouter *r)
{
    int64_t now;
    int i;
    if (!r || r->endpoint_count == 0) return false;

    now = lcn_router_now_ms();
    for (i = 0; i < r->endpoint_count; i++) {
        if (r->endpoints[i].last_check_ms == 0) return true;
        if (now - r->endpoints[i].last_check_ms >= r->health_check_interval_ms)
            return true;
    }
    return false;
}

static void router_check_single(LcnRouter *r, int idx)
{
    LcnRouterEndpoint *ep = &r->endpoints[idx];
    LcnHealthStatus old_status = ep->health_status;
    int64_t start_ms = lcn_router_now_ms();
    int64_t elapsed;

    LcnHttpResponse *resp = lcn_http_get(ep->endpoint);
    elapsed = lcn_router_now_ms() - start_ms;

    ep->last_check_ms = lcn_router_now_ms();

    if (resp && resp->status_code >= 200 && resp->status_code < 500) {
        /* Endpoint responded — mark healthy */
        ep->health_status = LCN_HEALTH_HEALTHY;
        ep->consecutive_failures = 0;

        /* Update rolling average latency (exponential moving average) */
        if (ep->avg_latency_ms == 0) {
            ep->avg_latency_ms = elapsed;
        } else {
            ep->avg_latency_ms = (ep->avg_latency_ms * 7 + elapsed) / 8;
        }

        /* Fire recovery callback if transitioned from unhealthy */
        if (old_status == LCN_HEALTH_UNHEALTHY && r->on_recovery) {
            r->on_recovery(r->name, ep->name, old_status, LCN_HEALTH_HEALTHY);
        }
    } else {
        /* Endpoint failed — possibly mark unhealthy */
        ep->consecutive_failures++;
        ep->total_failures++;

        if (ep->consecutive_failures >= r->max_consecutive_failures) {
            ep->health_status = LCN_HEALTH_UNHEALTHY;

            /* Fire unhealthy callback if transitioned */
            if (old_status != LCN_HEALTH_UNHEALTHY && r->on_unhealthy) {
                r->on_unhealthy(r->name, ep->name, old_status, LCN_HEALTH_UNHEALTHY);
            }
        }
    }

    if (resp) lcn_http_response_free(resp);
}

void lcn_router_health_check(LcnRouter *r)
{
    int i;
    if (!r) return;

    for (i = 0; i < r->endpoint_count; i++) {
        router_check_single(r, i);
    }
}

/* ════════════════════════════════════════════════
 * Result Reporting
 * ════════════════════════════════════════════════ */

void lcn_router_report_result(LcnRouter *r, int endpoint_idx,
                               bool success, int64_t latency_ms)
{
    LcnRouterEndpoint *ep;
    LcnHealthStatus old_status;

    if (!r || endpoint_idx < 0 || endpoint_idx >= r->endpoint_count) return;

    ep = &r->endpoints[endpoint_idx];
    old_status = ep->health_status;
    ep->total_calls++;

    if (success) {
        ep->consecutive_failures = 0;

        /* Update rolling average latency */
        if (ep->avg_latency_ms == 0) {
            ep->avg_latency_ms = latency_ms;
        } else {
            ep->avg_latency_ms = (ep->avg_latency_ms * 7 + latency_ms) / 8;
        }

        /* If was unhealthy, mark healthy and fire recovery */
        if (ep->health_status == LCN_HEALTH_UNHEALTHY) {
            ep->health_status = LCN_HEALTH_HEALTHY;
            if (r->on_recovery) {
                r->on_recovery(r->name, ep->name, old_status, LCN_HEALTH_HEALTHY);
            }
        } else if (ep->health_status == LCN_HEALTH_UNKNOWN) {
            ep->health_status = LCN_HEALTH_HEALTHY;
        }
    } else {
        ep->total_failures++;
        ep->consecutive_failures++;

        if (ep->consecutive_failures >= r->max_consecutive_failures) {
            ep->health_status = LCN_HEALTH_UNHEALTHY;
            if (old_status != LCN_HEALTH_UNHEALTHY && r->on_unhealthy) {
                r->on_unhealthy(r->name, ep->name, old_status, LCN_HEALTH_UNHEALTHY);
            }
        }
    }
}

/* ════════════════════════════════════════════════
 * Selection Strategies
 * ════════════════════════════════════════════════ */

/* Check if endpoint is selectable (not unhealthy). */
static bool ep_selectable(const LcnRouterEndpoint *ep)
{
    return ep->health_status != LCN_HEALTH_UNHEALTHY;
}

/* Strategy: lowest latency among healthy endpoints */
static LcnRouterEndpoint *select_by_latency(LcnRouter *r)
{
    LcnRouterEndpoint *best = NULL;
    int i;

    for (i = 0; i < r->endpoint_count; i++) {
        LcnRouterEndpoint *ep = &r->endpoints[i];
        if (!ep_selectable(ep)) continue;

        if (!best) {
            best = ep;
        } else if (ep->avg_latency_ms < best->avg_latency_ms) {
            best = ep;
        } else if (ep->avg_latency_ms == best->avg_latency_ms &&
                   ep->health_status == LCN_HEALTH_HEALTHY &&
                   best->health_status != LCN_HEALTH_HEALTHY) {
            best = ep;
        }
    }
    return best;
}

/* Strategy: lowest cost per token among healthy endpoints */
static LcnRouterEndpoint *select_by_cost(LcnRouter *r)
{
    LcnRouterEndpoint *best = NULL;
    int i;

    for (i = 0; i < r->endpoint_count; i++) {
        LcnRouterEndpoint *ep = &r->endpoints[i];
        if (!ep_selectable(ep)) continue;

        if (!best) {
            best = ep;
        } else if (ep->cost_per_token < best->cost_per_token) {
            best = ep;
        } else if (ep->cost_per_token == best->cost_per_token &&
                   ep->avg_latency_ms < best->avg_latency_ms) {
            best = ep;
        }
    }
    return best;
}

/* Strategy: round-robin among healthy endpoints */
static LcnRouterEndpoint *select_round_robin(LcnRouter *r)
{
    int checked = 0;
    while (checked < r->endpoint_count) {
        int idx = r->round_robin_idx % r->endpoint_count;
        r->round_robin_idx = (r->round_robin_idx + 1) % r->endpoint_count;

        if (ep_selectable(&r->endpoints[idx])) {
            return &r->endpoints[idx];
        }
        checked++;
    }
    return NULL; /* all unhealthy */
}

/* Strategy: failover — use first healthy, fall back to next */
static LcnRouterEndpoint *select_failover(LcnRouter *r)
{
    int i;
    for (i = 0; i < r->endpoint_count; i++) {
        if (ep_selectable(&r->endpoints[i])) {
            return &r->endpoints[i];
        }
    }
    return NULL;
}

LcnRouterEndpoint *lcn_router_select(LcnRouter *r)
{
    if (!r || r->endpoint_count == 0) return NULL;

    /* Auto health-check if due */
    if (lcn_router_health_check_due(r)) {
        lcn_router_health_check(r);
    }

    switch (r->strategy) {
    case LCN_ROUTE_STRATEGY_LATENCY:
        return select_by_latency(r);
    case LCN_ROUTE_STRATEGY_COST:
        return select_by_cost(r);
    case LCN_ROUTE_STRATEGY_ROUND_ROBIN:
        return select_round_robin(r);
    case LCN_ROUTE_STRATEGY_FAILOVER:
        return select_failover(r);
    default:
        return select_by_latency(r);
    }
}

/* ════════════════════════════════════════════════
 * Manual Health Management
 * ════════════════════════════════════════════════ */

void lcn_router_mark_unhealthy(LcnRouter *r, int endpoint_idx)
{
    LcnRouterEndpoint *ep;
    LcnHealthStatus old;

    if (!r || endpoint_idx < 0 || endpoint_idx >= r->endpoint_count) return;

    ep = &r->endpoints[endpoint_idx];
    old = ep->health_status;
    ep->health_status = LCN_HEALTH_UNHEALTHY;

    if (old != LCN_HEALTH_UNHEALTHY && r->on_unhealthy) {
        r->on_unhealthy(r->name, ep->name, old, LCN_HEALTH_UNHEALTHY);
    }
}

void lcn_router_mark_healthy(LcnRouter *r, int endpoint_idx)
{
    LcnRouterEndpoint *ep;
    LcnHealthStatus old;

    if (!r || endpoint_idx < 0 || endpoint_idx >= r->endpoint_count) return;

    ep = &r->endpoints[endpoint_idx];
    old = ep->health_status;
    ep->health_status = LCN_HEALTH_HEALTHY;
    ep->consecutive_failures = 0;

    if (old == LCN_HEALTH_UNHEALTHY && r->on_recovery) {
        r->on_recovery(r->name, ep->name, old, LCN_HEALTH_HEALTHY);
    }
}

/* ════════════════════════════════════════════════
 * Utility
 * ════════════════════════════════════════════════ */

int lcn_router_healthy_count(LcnRouter *r)
{
    int count = 0;
    int i;
    if (!r) return 0;

    for (i = 0; i < r->endpoint_count; i++) {
        if (r->endpoints[i].health_status == LCN_HEALTH_HEALTHY) {
            count++;
        }
    }
    return count;
}

void lcn_router_set_strategy(LcnRouter *r, LcnRouteStrategy strategy)
{
    if (!r) return;
    r->strategy = strategy;
    r->round_robin_idx = 0;
}

void lcn_router_set_callbacks(LcnRouter *r, LcnRouterCallback on_unhealthy,
                               LcnRouterCallback on_recovery)
{
    if (!r) return;
    r->on_unhealthy = on_unhealthy;
    r->on_recovery = on_recovery;
}

void lcn_router_reset_stats(LcnRouter *r)
{
    int i;
    if (!r) return;

    for (i = 0; i < r->endpoint_count; i++) {
        LcnRouterEndpoint *ep = &r->endpoints[i];
        ep->health_status = LCN_HEALTH_UNKNOWN;
        ep->last_check_ms = 0;
        ep->avg_latency_ms = 0;
        ep->total_calls = 0;
        ep->total_failures = 0;
        ep->consecutive_failures = 0;
    }
    r->round_robin_idx = 0;
}
