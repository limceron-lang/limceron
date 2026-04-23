/*
 * Limceron Runtime — A2A (Agent-to-Agent) Protocol Client
 *
 * Implements inter-agent communication over HTTP using JSON payloads.
 * Uses the existing Limceron HTTP client (http.c) for transport.
 *
 * A2A Protocol:
 *   - Message send:  POST <url>  {"jsonrpc":"2.0","method":"message/send",
 *                                  "params":{"message":{"role":"user",
 *                                  "parts":[{"type":"text","text":"..."}]}}}
 *   - Task send:     POST <url>  {"jsonrpc":"2.0","method":"tasks/send",
 *                                  "params":{"task":"..."}}
 *   - Agent card:    GET <url>/.well-known/agent.json
 *   - Ping:          GET <url>/health  (or root)
 *
 * Simplified response parsing:
 *   { "result": "...", "status": "ok|error" }
 *   Or full JSON-RPC 2.0 response.
 */

#include "a2a.h"
#include "http.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

/* ════════════════════════════════════════════════
 * Time utility
 * ════════════════════════════════════════════════ */

static int64_t a2a_now_ms(void)
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
 * String helpers
 * ════════════════════════════════════════════════ */

static char *a2a_strdup(const char *s)
{
    size_t len;
    char *dup;
    if (!s) return NULL;
    len = strlen(s);
    dup = (char *)malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

/* Simple JSON string escaping (escapes \, ", \n, \r, \t) */
static char *a2a_json_escape(const char *s)
{
    size_t i, j, slen, cap;
    char *out;
    if (!s) return a2a_strdup("");

    slen = strlen(s);
    cap = slen * 2 + 1;
    out = (char *)malloc(cap);
    if (!out) return NULL;

    for (i = 0, j = 0; i < slen; i++) {
        if (j + 3 >= cap) {
            cap *= 2;
            out = (char *)realloc(out, cap);
            if (!out) return NULL;
        }
        switch (s[i]) {
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:   out[j++] = s[i]; break;
        }
    }
    out[j] = '\0';
    return out;
}

/* ════════════════════════════════════════════════
 * Build extra headers string
 * ════════════════════════════════════════════════ */

static char *a2a_build_headers(LcnA2AClient *client)
{
    char buf[4096];
    size_t pos = 0;
    int i;

    buf[0] = '\0';

    /* Auth header */
    if (client->auth_token && client->auth_token[0]) {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                                "Authorization: Bearer %s\r\n", client->auth_token);
    }

    /* Custom headers */
    for (i = 0; i < client->header_count; i++) {
        if (client->headers[i].name && client->headers[i].value) {
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                                    "%s: %s\r\n",
                                    client->headers[i].name,
                                    client->headers[i].value);
        }
    }

    if (pos == 0) return NULL;
    return a2a_strdup(buf);
}

/* ════════════════════════════════════════════════
 * Parse A2A response body
 * ════════════════════════════════════════════════ */

static LcnA2AResponse *a2a_parse_response(LcnHttpResponse *http_resp)
{
    LcnA2AResponse *resp = (LcnA2AResponse *)calloc(1, sizeof(LcnA2AResponse));
    if (!resp) return NULL;

    resp->http_status = http_resp->status_code;
    resp->raw_body = http_resp->body ? a2a_strdup(http_resp->body) : NULL;

    if (http_resp->status_code < 200 || http_resp->status_code >= 300) {
        resp->ok = false;
        resp->status = a2a_strdup("error");
        {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf), "HTTP %d", http_resp->status_code);
            resp->error = a2a_strdup(errbuf);
        }
        return resp;
    }

    /* Try to parse JSON body */
    if (http_resp->body && http_resp->body_len > 0) {
        LcnJsonValue *json = lcn_json_parse(http_resp->body, http_resp->body_len);
        if (json) {
            /* Extract "result" field */
            {
                const char *result_str = lcn_json_get_string(json, "result");
                if (result_str) {
                    resp->result = a2a_strdup(result_str);
                } else {
                    LcnJsonValue *result_val = lcn_json_get(json, "result");
                    if (result_val && result_val->type == LCN_JSON_OBJECT) {
                        /* If result is an object, stringify it */
                        char *s = lcn_json_stringify(result_val);
                        resp->result = s;  /* already allocated */
                    }
                }
            }

            /* Extract "status" field */
            {
                const char *status_str = lcn_json_get_string(json, "status");
                if (status_str) {
                    resp->status = a2a_strdup(status_str);
                }
            }

            /* Extract "error" field if present */
            {
                const char *error_str = lcn_json_get_string(json, "error");
                if (error_str) {
                    resp->error = a2a_strdup(error_str);
                }
            }

            lcn_json_free(json);
        }

        /* Default status if not found in JSON */
        if (!resp->status) {
            resp->status = a2a_strdup("ok");
        }

        /* Default result to raw body if not parsed */
        if (!resp->result) {
            resp->result = a2a_strdup(http_resp->body);
        }
    } else {
        resp->status = a2a_strdup("ok");
        resp->result = a2a_strdup("");
    }

    resp->ok = (resp->status && strcmp(resp->status, "ok") == 0 && !resp->error);
    return resp;
}

/* ════════════════════════════════════════════════
 * Internal POST with retry
 * ════════════════════════════════════════════════ */

static LcnA2AResponse *a2a_post(LcnA2AClient *client, const char *url,
                                  const char *body)
{
    int attempts = 0;
    int max_attempts = 1 + (client->max_retries > 0 ? client->max_retries : 0);
    LcnA2AResponse *resp = NULL;
    char *extra_headers = a2a_build_headers(client);

    while (attempts < max_attempts) {
        int64_t start = a2a_now_ms();
        LcnHttpResponse *http_resp;
        int64_t elapsed;

        http_resp = lcn_http_post_with_headers(url, body,
                                                "application/json",
                                                extra_headers);
        elapsed = a2a_now_ms() - start;
        client->total_requests++;

        /* Update latency stats */
        if (client->avg_latency_ms == 0) {
            client->avg_latency_ms = elapsed;
        } else {
            client->avg_latency_ms = (client->avg_latency_ms * 7 + elapsed) / 8;
        }

        if (http_resp) {
            resp = a2a_parse_response(http_resp);
            lcn_http_response_free(http_resp);

            if (resp && resp->ok) {
                free(extra_headers);
                return resp;
            }

            /* If server error (5xx), retry */
            if (resp && resp->http_status >= 500) {
                lcn_a2a_response_free(resp);
                resp = NULL;
                client->total_errors++;
                attempts++;
                if (attempts < max_attempts && client->retry_delay_ms > 0) {
                    /* Simple busy-wait delay (no threads needed) */
                    int64_t wait_until = a2a_now_ms() + client->retry_delay_ms;
                    while (a2a_now_ms() < wait_until) { /* spin */ }
                }
                continue;
            }

            /* Non-retryable error — return as-is */
            free(extra_headers);
            return resp;
        }

        /* Connection failure — retry */
        client->total_errors++;
        attempts++;
        if (attempts < max_attempts && client->retry_delay_ms > 0) {
            int64_t wait_until = a2a_now_ms() + client->retry_delay_ms;
            while (a2a_now_ms() < wait_until) { /* spin */ }
        }
    }

    free(extra_headers);

    /* All attempts failed */
    resp = (LcnA2AResponse *)calloc(1, sizeof(LcnA2AResponse));
    if (resp) {
        resp->ok = false;
        resp->status = a2a_strdup("error");
        resp->error = a2a_strdup("connection failed after retries");
        resp->http_status = 0;
    }
    return resp;
}

/* ════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════ */

LcnA2AClient *lcn_a2a_connect(const char *url)
{
    return lcn_a2a_connect_with_auth(url, NULL, NULL);
}

LcnA2AClient *lcn_a2a_connect_with_auth(const char *url, const char *name,
                                          const char *auth_token)
{
    LcnA2AClient *client;
    size_t len;

    if (!url) return NULL;

    client = (LcnA2AClient *)calloc(1, sizeof(LcnA2AClient));
    if (!client) return NULL;

    /* Copy URL, stripping trailing slash */
    len = strlen(url);
    while (len > 0 && url[len - 1] == '/') len--;
    if (len >= LCN_A2A_MAX_URL) len = LCN_A2A_MAX_URL - 1;
    memcpy(client->agent_url, url, len);
    client->agent_url[len] = '\0';

    if (name) {
        size_t nlen = strlen(name);
        if (nlen >= LCN_A2A_MAX_NAME) nlen = LCN_A2A_MAX_NAME - 1;
        memcpy(client->agent_name, name, nlen);
        client->agent_name[nlen] = '\0';
    }

    client->auth_token = auth_token ? a2a_strdup(auth_token) : NULL;
    client->timeout_ms = 30000;
    client->max_retries = 2;
    client->retry_delay_ms = 1000;
    client->connected = false;
    client->header_count = 0;
    client->total_requests = 0;
    client->total_errors = 0;
    client->avg_latency_ms = 0;

    return client;
}

char *lcn_a2a_send(LcnA2AClient *client, const char *message)
{
    LcnA2AResponse *resp;
    char *result;
    char *escaped;
    char body[8192];

    if (!client || !message) return NULL;

    escaped = a2a_json_escape(message);
    if (!escaped) return NULL;

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"method\":\"message/send\","
             "\"params\":{\"message\":{\"role\":\"user\","
             "\"parts\":[{\"type\":\"text\",\"text\":\"%s\"}]}}}",
             escaped);
    free(escaped);

    resp = a2a_post(client, client->agent_url, body);
    if (!resp) return NULL;

    result = resp->result ? a2a_strdup(resp->result) : NULL;
    lcn_a2a_response_free(resp);
    return result;
}

char *lcn_a2a_send_task(LcnA2AClient *client, const char *task_json)
{
    LcnA2AResponse *resp;
    char *result;
    char body[8192];

    if (!client || !task_json) return NULL;

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\","
             "\"params\":{\"task\":%s}}",
             task_json);

    resp = a2a_post(client, client->agent_url, body);
    if (!resp) return NULL;

    result = resp->result ? a2a_strdup(resp->result) : NULL;
    lcn_a2a_response_free(resp);
    return result;
}

LcnA2AResponse *lcn_a2a_send_full(LcnA2AClient *client, const char *message)
{
    char *escaped;
    char body[8192];

    if (!client || !message) return NULL;

    escaped = a2a_json_escape(message);
    if (!escaped) return NULL;

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"method\":\"message/send\","
             "\"params\":{\"message\":{\"role\":\"user\","
             "\"parts\":[{\"type\":\"text\",\"text\":\"%s\"}]}}}",
             escaped);
    free(escaped);

    return a2a_post(client, client->agent_url, body);
}

LcnA2AResponse *lcn_a2a_send_task_full(LcnA2AClient *client, const char *task_json)
{
    char body[8192];

    if (!client || !task_json) return NULL;

    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"method\":\"tasks/send\","
             "\"params\":{\"task\":%s}}",
             task_json);

    return a2a_post(client, client->agent_url, body);
}

void lcn_a2a_disconnect(LcnA2AClient *client)
{
    int i;
    if (!client) return;

    free(client->auth_token);
    for (i = 0; i < client->header_count; i++) {
        free(client->headers[i].name);
        free(client->headers[i].value);
    }
    free(client);
}

void lcn_a2a_response_free(LcnA2AResponse *resp)
{
    if (!resp) return;
    free(resp->result);
    free(resp->status);
    free(resp->error);
    free(resp->raw_body);
    free(resp);
}

void lcn_a2a_add_header(LcnA2AClient *client, const char *name, const char *value)
{
    if (!client || !name || !value) return;
    if (client->header_count >= LCN_A2A_MAX_HEADERS) return;

    client->headers[client->header_count].name = a2a_strdup(name);
    client->headers[client->header_count].value = a2a_strdup(value);
    client->header_count++;
}

void lcn_a2a_set_auth(LcnA2AClient *client, const char *token)
{
    if (!client) return;
    free(client->auth_token);
    client->auth_token = token ? a2a_strdup(token) : NULL;
}

void lcn_a2a_set_timeout(LcnA2AClient *client, int timeout_ms)
{
    if (!client) return;
    client->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
}

void lcn_a2a_set_retries(LcnA2AClient *client, int max_retries, int retry_delay_ms)
{
    if (!client) return;
    client->max_retries = max_retries >= 0 ? max_retries : 0;
    client->retry_delay_ms = retry_delay_ms >= 0 ? retry_delay_ms : 0;
}

bool lcn_a2a_ping(LcnA2AClient *client)
{
    LcnHttpResponse *resp;
    bool ok;

    if (!client) return false;

    resp = lcn_http_get(client->agent_url);
    if (!resp) {
        client->connected = false;
        return false;
    }

    ok = (resp->status_code >= 200 && resp->status_code < 500);
    lcn_http_response_free(resp);

    client->connected = ok;
    return ok;
}

char *lcn_a2a_get_agent_card(LcnA2AClient *client)
{
    char url[LCN_A2A_MAX_URL + 64];
    LcnHttpResponse *resp;
    char *body;

    if (!client) return NULL;

    snprintf(url, sizeof(url), "%s/.well-known/agent.json", client->agent_url);

    resp = lcn_http_get(url);
    if (!resp) return NULL;

    if (resp->status_code != 200 || !resp->body) {
        lcn_http_response_free(resp);
        return NULL;
    }

    body = a2a_strdup(resp->body);
    lcn_http_response_free(resp);
    return body;
}
