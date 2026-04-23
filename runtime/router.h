/*
 * Limceron Runtime — Inference Router with Health Checking
 *
 * Provides model/endpoint selection with health monitoring,
 * latency tracking, and cost-aware routing.
 */

#ifndef LCN_ROUTER_H
#define LCN_ROUTER_H

#include <stdint.h>
#include <stdbool.h>

/* ════════════════════════════════════════════════
 * Router Endpoint
 * ════════════════════════════════════════════════ */

#define LCN_ROUTER_MAX_ENDPOINTS  32
#define LCN_ROUTER_MAX_NAME       128

typedef enum {
    LCN_HEALTH_UNKNOWN   =  0,
    LCN_HEALTH_HEALTHY   =  1,
    LCN_HEALTH_UNHEALTHY = -1
} LcnHealthStatus;

typedef struct {
    char        name[LCN_ROUTER_MAX_NAME];
    char        endpoint[1024];
    LcnHealthStatus health_status;
    int64_t     last_check_ms;
    int64_t     avg_latency_ms;
    double      cost_per_token;
    int         total_calls;
    int         total_failures;
    int         consecutive_failures;
} LcnRouterEndpoint;

/* ════════════════════════════════════════════════
 * Router
 * ════════════════════════════════════════════════ */

typedef enum {
    LCN_ROUTE_STRATEGY_LATENCY,     /* prefer lowest latency */
    LCN_ROUTE_STRATEGY_COST,        /* prefer cheapest */
    LCN_ROUTE_STRATEGY_ROUND_ROBIN, /* cycle through healthy */
    LCN_ROUTE_STRATEGY_FAILOVER     /* use first healthy, failover to next */
} LcnRouteStrategy;

typedef void (*LcnRouterCallback)(const char *router_name, const char *endpoint_name,
                                   LcnHealthStatus old_status, LcnHealthStatus new_status);

typedef struct {
    char                name[LCN_ROUTER_MAX_NAME];
    LcnRouterEndpoint   endpoints[LCN_ROUTER_MAX_ENDPOINTS];
    int                 endpoint_count;
    int64_t             health_check_interval_ms;  /* default: 30000 (30s) */
    LcnRouteStrategy    strategy;
    int                 round_robin_idx;           /* for round-robin */
    int                 max_consecutive_failures;  /* mark unhealthy after N failures (default: 3) */
    LcnRouterCallback   on_unhealthy;
    LcnRouterCallback   on_recovery;
} LcnRouter;

/* ════════════════════════════════════════════════
 * API
 * ════════════════════════════════════════════════ */

/* Create a new router with a name. Caller must free with lcn_router_free(). */
LcnRouter *lcn_router_new(const char *name);

/* Free a router and its resources. */
void lcn_router_free(LcnRouter *r);

/* Add an endpoint to the router. Returns index, or -1 on failure. */
int lcn_router_add_endpoint(LcnRouter *r, const char *name, const char *url, double cost_per_token);

/* Select the best endpoint based on strategy. Returns NULL if all unhealthy. */
LcnRouterEndpoint *lcn_router_select(LcnRouter *r);

/* Run health checks on all endpoints (HTTP GET to each endpoint). */
void lcn_router_health_check(LcnRouter *r);

/* Report the result of a call to an endpoint (updates stats). */
void lcn_router_report_result(LcnRouter *r, int endpoint_idx, bool success, int64_t latency_ms);

/* Get current time in milliseconds (monotonic). */
int64_t lcn_router_now_ms(void);

/* Set the routing strategy. */
void lcn_router_set_strategy(LcnRouter *r, LcnRouteStrategy strategy);

/* Set health check callbacks. */
void lcn_router_set_callbacks(LcnRouter *r, LcnRouterCallback on_unhealthy,
                               LcnRouterCallback on_recovery);

/* Get endpoint by index. Returns NULL if out of range. */
LcnRouterEndpoint *lcn_router_get_endpoint(LcnRouter *r, int idx);

/* Get endpoint by name. Returns NULL if not found. */
LcnRouterEndpoint *lcn_router_find_endpoint(LcnRouter *r, const char *name);

/* Return count of healthy endpoints. */
int lcn_router_healthy_count(LcnRouter *r);

/* Check if health check is due (based on interval and last check time). */
bool lcn_router_health_check_due(LcnRouter *r);

/* Mark an endpoint as unhealthy manually. */
void lcn_router_mark_unhealthy(LcnRouter *r, int endpoint_idx);

/* Mark an endpoint as healthy manually. */
void lcn_router_mark_healthy(LcnRouter *r, int endpoint_idx);

/* Reset all endpoint statistics. */
void lcn_router_reset_stats(LcnRouter *r);

#endif /* LCN_ROUTER_H */
