/*
 * Limceron Dashboard REST API
 * Route handlers for the agent dashboard backend.
 * Pure C99, single-threaded (Stage 0).
 */
#ifndef LCN_DASHBOARD_API_H
#define LCN_DASHBOARD_API_H

#include "httpd.h"

/* Register all dashboard API routes with the server */
void lcn_dashboard_register_routes(LcnHttpServer *server);

/* Individual endpoint handlers */
void lcn_api_health(const LcnHttpRequest *req);
void lcn_api_agents_list(const LcnHttpRequest *req);
void lcn_api_agents_detail(const LcnHttpRequest *req);
void lcn_api_events(const LcnHttpRequest *req);
void lcn_api_events_stream(const LcnHttpRequest *req);
void lcn_api_metrics(const LcnHttpRequest *req);
void lcn_api_budget(const LcnHttpRequest *req);
void lcn_api_guards(const LcnHttpRequest *req);
void lcn_api_capabilities(const LcnHttpRequest *req);
void lcn_api_agent_pause(const LcnHttpRequest *req);
void lcn_api_agent_resume(const LcnHttpRequest *req);

/* Memory endpoints */
void lcn_api_memory_list(const LcnHttpRequest *req);
void lcn_api_memory_search(const LcnHttpRequest *req);
void lcn_api_memory_sessions(const LcnHttpRequest *req);
void lcn_api_memory_store(const LcnHttpRequest *req);
void lcn_api_memory_delete(const LcnHttpRequest *req);

/* Knowledge base endpoints */
void lcn_api_kb_search(const LcnHttpRequest *req);
void lcn_api_kb_status(const LcnHttpRequest *req);

#endif /* LCN_DASHBOARD_API_H */
