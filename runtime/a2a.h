/*
 * Limceron Runtime — A2A (Agent-to-Agent) Protocol Client
 *
 * Implements the A2A protocol for inter-agent communication.
 * Uses HTTP POST with JSON payloads to communicate with external agents.
 *
 * Protocol:
 *   Request:  POST <agent_url> { "task": "...", "message": "..." }
 *   Response: { "result": "...", "status": "ok|error" }
 */

#ifndef LCN_A2A_H
#define LCN_A2A_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ════════════════════════════════════════════════
 * A2A Client
 * ════════════════════════════════════════════════ */

#define LCN_A2A_MAX_HEADERS  16
#define LCN_A2A_MAX_URL      2048
#define LCN_A2A_MAX_NAME     128

typedef struct {
    char    agent_url[LCN_A2A_MAX_URL];
    char    agent_name[LCN_A2A_MAX_NAME];
    char   *auth_token;         /* Bearer token for authentication (NULL = no auth) */
    int     timeout_ms;         /* Request timeout in milliseconds (default: 30000) */
    int     max_retries;        /* Max retries on failure (default: 2) */
    int     retry_delay_ms;     /* Delay between retries (default: 1000) */
    bool    connected;          /* Whether connection has been verified */
    /* Custom headers */
    struct {
        char *name;
        char *value;
    } headers[LCN_A2A_MAX_HEADERS];
    int     header_count;
    /* Stats */
    int     total_requests;
    int     total_errors;
    int64_t avg_latency_ms;
} LcnA2AClient;

/* ════════════════════════════════════════════════
 * A2A Response
 * ════════════════════════════════════════════════ */

typedef struct {
    char   *result;         /* Result string (caller must free) */
    char   *status;         /* "ok" or "error" (caller must free) */
    char   *error;          /* Error message if status is "error" (caller must free) */
    char   *raw_body;       /* Raw response body (caller must free) */
    int     http_status;    /* HTTP status code */
    bool    ok;             /* true if status == "ok" and HTTP 2xx */
} LcnA2AResponse;

/* ════════════════════════════════════════════════
 * API
 * ════════════════════════════════════════════════ */

/* Connect to a remote agent. Returns client handle. Caller must free with lcn_a2a_disconnect(). */
LcnA2AClient *lcn_a2a_connect(const char *url);

/* Connect with a name and auth token. */
LcnA2AClient *lcn_a2a_connect_with_auth(const char *url, const char *name,
                                          const char *auth_token);

/* Send a simple text message. Returns allocated string (caller must free). */
char *lcn_a2a_send(LcnA2AClient *client, const char *message);

/* Send a task with JSON payload. Returns allocated string (caller must free). */
char *lcn_a2a_send_task(LcnA2AClient *client, const char *task_json);

/* Send a message and get full response struct. Caller must free with lcn_a2a_response_free(). */
LcnA2AResponse *lcn_a2a_send_full(LcnA2AClient *client, const char *message);

/* Send a task and get full response struct. Caller must free with lcn_a2a_response_free(). */
LcnA2AResponse *lcn_a2a_send_task_full(LcnA2AClient *client, const char *task_json);

/* Disconnect and free client resources. */
void lcn_a2a_disconnect(LcnA2AClient *client);

/* Free a response struct. */
void lcn_a2a_response_free(LcnA2AResponse *resp);

/* Add a custom header to the client. */
void lcn_a2a_add_header(LcnA2AClient *client, const char *name, const char *value);

/* Set authentication token. */
void lcn_a2a_set_auth(LcnA2AClient *client, const char *token);

/* Set timeout in milliseconds. */
void lcn_a2a_set_timeout(LcnA2AClient *client, int timeout_ms);

/* Set retry policy. */
void lcn_a2a_set_retries(LcnA2AClient *client, int max_retries, int retry_delay_ms);

/* Check if the remote agent is reachable (GET to agent URL). */
bool lcn_a2a_ping(LcnA2AClient *client);

/* Get agent card (GET to .well-known/agent.json relative to agent URL). */
char *lcn_a2a_get_agent_card(LcnA2AClient *client);

#endif /* LCN_A2A_H */
