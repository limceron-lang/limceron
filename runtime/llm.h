/*
 * Limceron LLM Inference Client
 * OpenAI-compatible chat completions API (works with Ollama, vLLM, etc.)
 * C99, no external dependencies beyond Limceron runtime.
 */
#ifndef LCN_LLM_H
#define LCN_LLM_H

#include "json.h"
#include <stdint.h>
#include <stdbool.h>

/* LLM request configuration */
typedef struct {
    const char *endpoint;       /* Base URL, default "http://localhost:11434" */
    const char *model;          /* Model name, e.g. "llama3" */
    const char *system_prompt;  /* Optional system prompt */
    const char *api_key;        /* API key — sent as "Authorization: Bearer <key>" header */
    const char **messages;      /* Alternating role/content pairs: ["user","hi","assistant","hello",...] */
    int         message_count;  /* Number of strings in messages array (must be even) */
    double      temperature;    /* 0.0-2.0, default 0.7 */
    int64_t     max_tokens;     /* Max tokens to generate, 0=model default */

    /* Tool calling (optional) */
    const char **tool_names;    /* Array of tool name strings */
    const char **tool_descriptions;
    const char **tool_schemas;  /* JSON schema strings for each tool's parameters */
    int         tool_count;
} LcnLlmRequest;

/* LLM response */
typedef struct {
    char    *content;           /* Generated text (malloc'd, caller frees) */
    int64_t  prompt_tokens;
    int64_t  completion_tokens;
    int64_t  total_tokens;

    /* Tool calls (if model requested tool use) */
    struct {
        char *name;
        char *arguments;        /* JSON string of arguments */
    } *tool_calls;
    int tool_call_count;

    /* Entropy (Shannon entropy of first token logprobs) */
    double   entropy;           /* H = -sum(p * log2(p)) */
    double   confidence;        /* 1.0 - normalized_entropy [0,1] */
    double  *logprobs;          /* Raw log probabilities (malloc'd, caller frees) */
    int      logprob_count;

    /* Error info */
    bool    ok;
    char   *error;              /* Error message if !ok */
} LcnLlmResponse;

/* Default endpoint */
#define LCN_LLM_DEFAULT_ENDPOINT "http://localhost:11434"

/* Create a default request */
LcnLlmRequest lcn_llm_request_new(const char *model);

/* Send a completion request. Returns response (caller must free with lcn_llm_response_free). */
LcnLlmResponse *lcn_llm_complete(const LcnLlmRequest *req);

/* Free response */
void lcn_llm_response_free(LcnLlmResponse *resp);

#endif /* LCN_LLM_H */
