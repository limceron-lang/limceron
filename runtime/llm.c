/*
 * Limceron LLM Inference Client — Implementation
 * OpenAI-compatible chat completions API (Ollama, vLLM, LM Studio, etc.)
 *
 * Sends POST to {endpoint}/v1/chat/completions with JSON payload,
 * parses the response, and returns a LcnLlmResponse.
 *
 * C99, -Wall -Wextra -Werror -pedantic clean.
 */

#include "llm.h"
#include "json.h"
#include "http.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/* Safe strdup — returns NULL when src is NULL. */
static char *llm_strdup(const char *src)
{
    char *dst;
    size_t len;
    if (!src) {
        return NULL;
    }
    len = strlen(src);
    dst = (char *)malloc(len + 1);
    if (dst) {
        memcpy(dst, src, len + 1);
    }
    return dst;
}

/* Allocate a response pre-filled as an error. */
static LcnLlmResponse *llm_error_response(const char *message)
{
    LcnLlmResponse *resp = (LcnLlmResponse *)calloc(1, sizeof(LcnLlmResponse));
    if (!resp) {
        return NULL;
    }
    resp->ok    = false;
    resp->error = llm_strdup(message);
    return resp;
}

/* Concatenate base URL + path.  Caller must free(). */
static char *llm_build_url(const char *endpoint, const char *path)
{
    size_t elen, plen;
    char  *url;
    int    strip;

    if (!endpoint || !path) {
        return NULL;
    }

    elen  = strlen(endpoint);
    plen  = strlen(path);
    strip = (elen > 0 && endpoint[elen - 1] == '/') ? 1 : 0;

    url = (char *)malloc(elen - (size_t)strip + plen + 1);
    if (!url) {
        return NULL;
    }
    memcpy(url, endpoint, elen - (size_t)strip);
    memcpy(url + elen - (size_t)strip, path, plen + 1);
    return url;
}

/* --------------------------------------------------------------------------
 * Build the messages JSON array from the request.
 *
 * Layout:
 *   1. If system_prompt is set  -> {"role":"system","content":"..."}
 *   2. Alternating role/content pairs from req->messages
 * -------------------------------------------------------------------------- */
static LcnJsonValue *llm_build_messages(const LcnLlmRequest *req)
{
    LcnJsonValue *arr = lcn_json_array_new();
    int i;

    if (!arr) {
        return NULL;
    }

    /* Optional system prompt */
    if (req->system_prompt && req->system_prompt[0] != '\0') {
        LcnJsonValue *msg = lcn_json_object_new();
        if (msg) {
            lcn_json_set_string(msg, "role",    "system");
            lcn_json_set_string(msg, "content", req->system_prompt);
            lcn_json_array_push(arr, msg);
        }
    }

    /* Conversation messages (pairs of role, content) */
    if (req->messages && req->message_count >= 2) {
        for (i = 0; i + 1 < req->message_count; i += 2) {
            LcnJsonValue *msg = lcn_json_object_new();
            if (msg) {
                lcn_json_set_string(msg, "role",    req->messages[i]);
                lcn_json_set_string(msg, "content", req->messages[i + 1]);
                lcn_json_array_push(arr, msg);
            }
        }
    }

    return arr;
}

/* --------------------------------------------------------------------------
 * Build the tools JSON array (OpenAI function-calling format).
 *
 * Each entry:
 * {
 *   "type": "function",
 *   "function": {
 *     "name": "...",
 *     "description": "...",
 *     "parameters": { ... }       <-- parsed from tool_schemas[i]
 *   }
 * }
 * -------------------------------------------------------------------------- */
static LcnJsonValue *llm_build_tools(const LcnLlmRequest *req)
{
    LcnJsonValue *arr;
    int i;

    if (req->tool_count <= 0 || !req->tool_names) {
        return NULL;
    }

    arr = lcn_json_array_new();
    if (!arr) {
        return NULL;
    }

    for (i = 0; i < req->tool_count; i++) {
        LcnJsonValue *tool     = lcn_json_object_new();
        LcnJsonValue *func_obj = lcn_json_object_new();

        if (!tool || !func_obj) {
            lcn_json_free(tool);
            lcn_json_free(func_obj);
            continue;
        }

        lcn_json_set_string(func_obj, "name", req->tool_names[i]);

        if (req->tool_descriptions && req->tool_descriptions[i]) {
            lcn_json_set_string(func_obj, "description", req->tool_descriptions[i]);
        }

        /* Parse the JSON schema string into a value and attach it. */
        if (req->tool_schemas && req->tool_schemas[i]) {
            LcnJsonValue *schema = lcn_json_parse(
                req->tool_schemas[i],
                strlen(req->tool_schemas[i])
            );
            if (schema) {
                lcn_json_set(func_obj, "parameters", schema);
            }
        }

        lcn_json_set_string(tool, "type", "function");
        lcn_json_set(tool, "function", func_obj);
        lcn_json_array_push(arr, tool);
    }

    return arr;
}

/* --------------------------------------------------------------------------
 * Build the full request body JSON.
 * -------------------------------------------------------------------------- */
static char *llm_build_request_body(const LcnLlmRequest *req)
{
    LcnJsonValue *root;
    LcnJsonValue *messages;
    LcnJsonValue *tools;
    char *body;

    root = lcn_json_object_new();
    if (!root) {
        return NULL;
    }

    /* Model (required) */
    lcn_json_set_string(root, "model", req->model ? req->model : "llama3");

    /* Messages array */
    messages = llm_build_messages(req);
    if (messages) {
        lcn_json_set(root, "messages", messages);
    }

    /* Temperature */
    lcn_json_set_number(root, "temperature", req->temperature);

    /* Max tokens (only include when explicitly requested) */
    if (req->max_tokens > 0) {
        lcn_json_set_number(root, "max_tokens", (double)req->max_tokens);
    }

    /* Tools (optional) */
    tools = llm_build_tools(req);
    if (tools) {
        lcn_json_set(root, "tools", tools);
    }

    /* Request logprobs for entropy calculation */
    lcn_json_set_bool(root, "logprobs", true);
    lcn_json_set_number(root, "top_logprobs", 5.0);

    body = lcn_json_stringify(root);
    lcn_json_free(root);
    return body;
}

/* --------------------------------------------------------------------------
 * Parse tool_calls from the response message object.
 *
 * Expected format:
 * "tool_calls": [
 *   { "function": { "name": "...", "arguments": "..." } },
 *   ...
 * ]
 * -------------------------------------------------------------------------- */
static void llm_parse_tool_calls(LcnLlmResponse *resp, const LcnJsonValue *message)
{
    const LcnJsonValue *tc_arr;
    size_t tc_len;
    size_t i;
    int    filled;

    tc_arr = lcn_json_get(message, "tool_calls");
    if (!tc_arr || tc_arr->type != LCN_JSON_ARRAY) {
        return;
    }

    tc_len = lcn_json_array_len(tc_arr);
    if (tc_len == 0) {
        return;
    }

    resp->tool_calls = calloc(tc_len, sizeof(resp->tool_calls[0]));
    if (!resp->tool_calls) {
        return;
    }

    filled = 0;
    for (i = 0; i < tc_len; i++) {
        const LcnJsonValue *tc_item = lcn_json_array_get(tc_arr, i);
        const LcnJsonValue *func;
        const char *name;
        const char *args;

        if (!tc_item) {
            continue;
        }

        func = lcn_json_get(tc_item, "function");
        if (!func) {
            continue;
        }

        name = lcn_json_get_string(func, "name");
        args = lcn_json_get_string(func, "arguments");

        resp->tool_calls[filled].name      = llm_strdup(name);
        resp->tool_calls[filled].arguments  = llm_strdup(args);
        filled++;
    }

    resp->tool_call_count = filled;
}

/* --------------------------------------------------------------------------
 * Parse the full HTTP response body into a LcnLlmResponse.
 *
 * Expected top-level structure:
 * {
 *   "choices": [ { "message": { "content": "...", "tool_calls": [...] } } ],
 *   "usage":   { "prompt_tokens": N, "completion_tokens": N, "total_tokens": N }
 * }
 * -------------------------------------------------------------------------- */
static LcnLlmResponse *llm_parse_response(const char *body, size_t body_len)
{
    LcnLlmResponse     *resp;
    LcnJsonValue       *root;
    const LcnJsonValue *choices;
    const LcnJsonValue *first_choice;
    const LcnJsonValue *message;
    const LcnJsonValue *usage;
    const char          *content;

    if (!body || body_len == 0) {
        return llm_error_response("empty response body");
    }

    root = lcn_json_parse(body, body_len);
    if (!root) {
        return llm_error_response("failed to parse response JSON");
    }

    /* Check for API-level error field */
    {
        const char *api_err = lcn_json_get_string(root, "error");
        if (api_err) {
            LcnLlmResponse *err_resp = llm_error_response(api_err);
            lcn_json_free(root);
            return err_resp;
        }
    }

    /* Also handle {"error": {"message": "..."}} form */
    {
        const LcnJsonValue *err_obj = lcn_json_get(root, "error");
        if (err_obj && err_obj->type == LCN_JSON_OBJECT) {
            const char *err_msg = lcn_json_get_string(err_obj, "message");
            LcnLlmResponse *err_resp = llm_error_response(
                err_msg ? err_msg : "unknown API error"
            );
            lcn_json_free(root);
            return err_resp;
        }
    }

    /* Navigate to choices[0].message */
    choices = lcn_json_get(root, "choices");
    if (!choices || choices->type != LCN_JSON_ARRAY || lcn_json_array_len(choices) == 0) {
        lcn_json_free(root);
        return llm_error_response("response missing 'choices' array");
    }

    first_choice = lcn_json_array_get(choices, 0);
    if (!first_choice) {
        lcn_json_free(root);
        return llm_error_response("first choice is null");
    }

    message = lcn_json_get(first_choice, "message");
    if (!message) {
        lcn_json_free(root);
        return llm_error_response("first choice missing 'message'");
    }

    /* Allocate successful response */
    resp = (LcnLlmResponse *)calloc(1, sizeof(LcnLlmResponse));
    if (!resp) {
        lcn_json_free(root);
        return NULL;
    }
    resp->ok = true;

    /* Content (may be null when the model only returns tool calls) */
    content = lcn_json_get_string(message, "content");
    resp->content = llm_strdup(content);

    /* Tool calls */
    llm_parse_tool_calls(resp, message);

    /* Usage statistics */
    usage = lcn_json_get(root, "usage");
    if (usage) {
        resp->prompt_tokens     = (int64_t)lcn_json_get_number(usage, "prompt_tokens");
        resp->completion_tokens = (int64_t)lcn_json_get_number(usage, "completion_tokens");
        resp->total_tokens      = (int64_t)lcn_json_get_number(usage, "total_tokens");
    }

    /* Logprobs → Shannon Entropy calculation
     * OpenAI/vLLM format: choices[0].logprobs.content[0].top_logprobs[{token, logprob}]
     * We compute entropy over the top logprobs of the first completion token. */
    {
        const LcnJsonValue *lp_obj = lcn_json_get(first_choice, "logprobs");
        resp->entropy = 0.0;
        resp->confidence = 1.0;
        resp->logprobs = NULL;
        resp->logprob_count = 0;

        if (lp_obj && lp_obj->type == LCN_JSON_OBJECT) {
            const LcnJsonValue *lp_content = lcn_json_get(lp_obj, "content");
            if (lp_content && lp_content->type == LCN_JSON_ARRAY &&
                lcn_json_array_len(lp_content) > 0) {
                /* Get first token's top_logprobs */
                const LcnJsonValue *first_token = lcn_json_array_get(lp_content, 0);
                if (first_token) {
                    const LcnJsonValue *top_lp = lcn_json_get(first_token, "top_logprobs");
                    if (top_lp && top_lp->type == LCN_JSON_ARRAY) {
                        size_t n = lcn_json_array_len(top_lp);
                        if (n > 0) {
                            /* Extract log probabilities */
                            double *probs = (double *)malloc(sizeof(double) * n);
                            double sum_p = 0.0;
                            size_t i;
                            for (i = 0; i < n; i++) {
                                const LcnJsonValue *entry = lcn_json_array_get(top_lp, i);
                                double lp_val = lcn_json_get_number(entry, "logprob");
                                probs[i] = exp(lp_val);  /* logprob → probability */
                                sum_p += probs[i];
                            }
                            /* Normalize probabilities */
                            if (sum_p > 0.0) {
                                for (i = 0; i < n; i++) probs[i] /= sum_p;
                            }
                            /* Shannon entropy: H = -sum(p * log2(p)) */
                            double H = 0.0;
                            for (i = 0; i < n; i++) {
                                if (probs[i] > 1e-10) {
                                    H -= probs[i] * log2(probs[i]);
                                }
                            }
                            /* Normalize to [0, 1]: divide by max entropy log2(n) */
                            double max_H = log2((double)n);
                            double norm_H = (max_H > 0.0) ? H / max_H : 0.0;

                            resp->entropy = H;
                            resp->confidence = 1.0 - norm_H;
                            if (resp->confidence < 0.0) resp->confidence = 0.0;
                            if (resp->confidence > 1.0) resp->confidence = 1.0;
                            resp->logprobs = probs;
                            resp->logprob_count = (int)n;
                        }
                    }
                }
            }
        }
    }

    lcn_json_free(root);
    return resp;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

LcnLlmRequest lcn_llm_request_new(const char *model)
{
    LcnLlmRequest req;
    memset(&req, 0, sizeof(req));
    req.endpoint    = LCN_LLM_DEFAULT_ENDPOINT;
    req.model       = model;
    req.temperature = 0.7;
    return req;
}

LcnLlmResponse *lcn_llm_complete(const LcnLlmRequest *req)
{
    char             *url       = NULL;
    char             *body      = NULL;
    LcnHttpResponse *http_resp = NULL;
    LcnLlmResponse  *llm_resp  = NULL;

    if (!req) {
        return llm_error_response("request is NULL");
    }
    if (!req->model || req->model[0] == '\0') {
        return llm_error_response("model name is required");
    }

    /* 1. Build URL */
    url = llm_build_url(
        req->endpoint ? req->endpoint : LCN_LLM_DEFAULT_ENDPOINT,
        "/v1/chat/completions"
    );
    if (!url) {
        return llm_error_response("failed to allocate URL");
    }

    /* 2. Build request JSON body */
    body = llm_build_request_body(req);
    if (!body) {
        free(url);
        return llm_error_response("failed to build request JSON");
    }

    /* 3. Send HTTP POST (with API key if provided) */
    if (req->api_key && req->api_key[0]) {
        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header),
                 "Authorization: Bearer %s\r\n", req->api_key);
        http_resp = lcn_http_post_with_headers(url, body, "application/json",
                                                auth_header);
    } else {
        http_resp = lcn_http_post(url, body, "application/json");
    }
    free(url);
    free(body);

    if (!http_resp) {
        return llm_error_response("HTTP request failed (connection refused?)");
    }

    /* 4. Check HTTP status */
    if (http_resp->status_code < 200 || http_resp->status_code >= 300) {
        char err_buf[256];
        snprintf(err_buf, sizeof(err_buf),
                 "HTTP %d from LLM endpoint", http_resp->status_code);

        /* Try to extract a more useful error from the response body */
        if (http_resp->body && http_resp->body_len > 0) {
            LcnJsonValue *err_json = lcn_json_parse(
                http_resp->body, http_resp->body_len
            );
            if (err_json) {
                const char *msg = lcn_json_get_string(err_json, "error");
                if (!msg) {
                    const LcnJsonValue *err_obj = lcn_json_get(err_json, "error");
                    if (err_obj && err_obj->type == LCN_JSON_OBJECT) {
                        msg = lcn_json_get_string(err_obj, "message");
                    }
                }
                if (msg) {
                    snprintf(err_buf, sizeof(err_buf),
                             "HTTP %d: %.*s",
                             http_resp->status_code,
                             (int)(sizeof(err_buf) - 16), msg);
                }
                lcn_json_free(err_json);
            }
        }

        lcn_http_response_free(http_resp);
        return llm_error_response(err_buf);
    }

    /* 5. Parse response body */
    llm_resp = llm_parse_response(http_resp->body, http_resp->body_len);
    lcn_http_response_free(http_resp);
    return llm_resp;
}

/* --------------------------------------------------------------------------
 * Convenience wrapper: lcn_llm_call()
 * Simple interface for codegen — hides LcnLlmRequest/LcnLlmResponse.
 * Uses types from lcn_runtime.h (LcnLlmResult, LcnBudget).
 * -------------------------------------------------------------------------- */

#include "lcn_runtime.h"

/* Global model registry for local ONNX models referenced by agent endpoint:"local" */
static LcnModel *_lcn_local_model = NULL;

void lcn_set_local_model(LcnModel *m) { _lcn_local_model = m; }
LcnModel *lcn_get_local_model(void) { return _lcn_local_model; }

LcnLlmResult lcn_llm_call(const char *endpoint, const char *model,
                             const char *system_prompt, const char *user_message,
                             LcnBudget *budget, const char *api_key)
{
    LcnLlmResult result;
    LcnLlmRequest req;
    LcnLlmResponse *resp;
    const char *messages[2];

    memset(&result, 0, sizeof(result));
    time_t _start = time(NULL);

    /* ── Local ONNX model intercept ──────────────────────
     * When endpoint is "local", route to the ONNX model
     * instead of making an HTTP call. This enables agents
     * with entropy_budget to use local models transparently.
     */
    if (endpoint && strcmp(endpoint, "local") == 0 && _lcn_local_model) {
        LcnModelResult mr = lcn_model_predict(_lcn_local_model, user_message);
        if (mr.ok) {
            result.ok = true;
            result.content = llm_strdup(mr.label ? mr.label : "");
            result.confidence = mr.confidence / 100.0;
            result.entropy = mr.entropy;
            /* no token usage for local models */
        } else {
            result.ok = false;
            result.error = llm_strdup(mr.error ? mr.error : "model predict failed");
        }
        return result;
    }

    /* Check budget before calling */
    if (budget) {
        bool budget_ok = true;
        if (budget->max_tokens > 0 && budget->used_tokens >= budget->max_tokens)
            budget_ok = false;
        if (budget->max_cost > 0.0 && budget->used_cost >= budget->max_cost)
            budget_ok = false;
        if (!budget_ok) {
            result.ok = false;
            result.error = llm_strdup("budget exhausted");
            return result;
        }
    }

    /* Build request */
    req = lcn_llm_request_new(model ? model : "llama3");

    /* Endpoint resolution: explicit param > env var > default (Ollama) */
    if (endpoint) {
        req.endpoint = endpoint;
    } else {
        const char *env_ep = getenv("LCN_LLM_ENDPOINT");
        if (!env_ep) env_ep = getenv("OLLAMA_HOST");
        if (env_ep && env_ep[0]) req.endpoint = env_ep;
    }
    req.system_prompt = system_prompt;
    req.api_key = api_key;

    /* Set up user message */
    messages[0] = "user";
    messages[1] = user_message ? user_message : "";
    req.messages = messages;
    req.message_count = 2;

    /* Make the call */
    lcn_emit_llm_request("", req.model, req.endpoint);
    resp = lcn_llm_complete(&req);
    if (!resp) {
        result.ok = false;
        result.error = llm_strdup("LLM request failed (connection refused?)");
        return result;
    }

    /* Copy response to result */
    result.ok = resp->ok;
    result.content = resp->content ? llm_strdup(resp->content) : NULL;
    result.error = resp->error ? llm_strdup(resp->error) : NULL;
    result.prompt_tokens = resp->prompt_tokens;
    result.completion_tokens = resp->completion_tokens;
    result.total_tokens = resp->total_tokens;
    result.entropy = resp->entropy;
    result.confidence = resp->confidence;

    /* Deduct budget */
    if (budget && resp->ok && resp->total_tokens > 0) {
        budget->used_tokens += resp->total_tokens;
    }

    lcn_llm_response_free(resp);

    {
        time_t _end = time(NULL);
        int64_t _elapsed_ms = (int64_t)difftime(_end, _start) * 1000;
        lcn_emit_llm_response("", result.total_tokens, 0.0, _elapsed_ms);
    }

    return result;
}

void lcn_llm_response_free(LcnLlmResponse *resp)
{
    int i;

    if (!resp) {
        return;
    }

    free(resp->content);
    free(resp->error);

    if (resp->tool_calls) {
        for (i = 0; i < resp->tool_call_count; i++) {
            free(resp->tool_calls[i].name);
            free(resp->tool_calls[i].arguments);
        }
        free(resp->tool_calls);
    }

    free(resp);
}
