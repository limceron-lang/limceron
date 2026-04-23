/*
 * Limceron MCP Server — Implementation
 * JSON-RPC 2.0 over stdin/stdout, MCP protocol.
 */
#include "mcp_server.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────── */

/* Write a JSON-RPC response line to stdout */
static void server_send(LcnJsonValue *msg) {
    char *str = lcn_json_stringify(msg);
    if (str) {
        fputs(str, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        free(str);
    }
}

/* Read a line from stdin. Returns malloc'd string or NULL on EOF. */
static char *server_read_line(void) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    int c;
    while ((c = fgetc(stdin)) != EOF) {
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }

    if (len == 0 && c == EOF) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    return buf;
}

/* Build JSON-RPC 2.0 response envelope */
static LcnJsonValue *make_response(LcnJsonValue *id, LcnJsonValue *result) {
    LcnJsonValue *resp = lcn_json_object_new();
    lcn_json_set_string(resp, "jsonrpc", "2.0");
    /* Copy id (don't embed — original is freed with the request msg) */
    if (id && id->type == LCN_JSON_NUMBER)
        lcn_json_set_number(resp, "id", id->data.number);
    else if (id && id->type == LCN_JSON_STRING)
        lcn_json_set_string(resp, "id", id->data.string.str);
    lcn_json_set(resp, "result", result);
    return resp;
}

static LcnJsonValue *make_error_response(LcnJsonValue *id, int code, const char *message) {
    LcnJsonValue *resp = lcn_json_object_new();
    lcn_json_set_string(resp, "jsonrpc", "2.0");
    if (id && id->type == LCN_JSON_NUMBER)
        lcn_json_set_number(resp, "id", id->data.number);
    else if (id && id->type == LCN_JSON_STRING)
        lcn_json_set_string(resp, "id", id->data.string.str);
    LcnJsonValue *err = lcn_json_object_new();
    lcn_json_set_number(err, "code", (double)code);
    lcn_json_set_string(err, "message", message);
    lcn_json_set(resp, "error", err);
    return resp;
}

/* ── Public API ──────────────────────────────────────────── */

void lcn_mcp_server_init(LcnMcpServer *server, const char *name, const char *version) {
    memset(server, 0, sizeof(*server));
    server->name = name;
    server->version = version;
}

void lcn_mcp_server_add_tool(LcnMcpServer *server, const char *name,
                              const char *description, const char *input_schema,
                              LcnMcpToolHandler handler) {
    if (server->tool_count >= 32) return;
    LcnMcpServerTool *t = &server->tools[server->tool_count++];
    t->name = name;
    t->description = description;
    t->input_schema = input_schema;
    t->handler = handler;
}

/* ── Request handlers ────────────────────────────────────── */

static void handle_initialize(LcnMcpServer *server, LcnJsonValue *id) {
    LcnJsonValue *result = lcn_json_object_new();
    lcn_json_set_string(result, "protocolVersion", "2024-11-05");

    LcnJsonValue *caps = lcn_json_object_new();
    lcn_json_set(caps, "tools", lcn_json_object_new());
    lcn_json_set(result, "capabilities", caps);

    LcnJsonValue *info = lcn_json_object_new();
    lcn_json_set_string(info, "name", server->name);
    lcn_json_set_string(info, "version", server->version);
    lcn_json_set(result, "serverInfo", info);

    LcnJsonValue *resp = make_response(id, result);
    server_send(resp);
    lcn_json_free(resp);
}

static void handle_tools_list(LcnMcpServer *server, LcnJsonValue *id) {
    LcnJsonValue *result = lcn_json_object_new();
    LcnJsonValue *tools = lcn_json_array_new();

    int i;
    for (i = 0; i < server->tool_count; i++) {
        LcnMcpServerTool *t = &server->tools[i];
        LcnJsonValue *tool = lcn_json_object_new();
        lcn_json_set_string(tool, "name", t->name);
        if (t->description)
            lcn_json_set_string(tool, "description", t->description);
        if (t->input_schema) {
            LcnJsonValue *schema = lcn_json_parse(t->input_schema, strlen(t->input_schema));
            if (schema) lcn_json_set(tool, "inputSchema", schema);
        }
        lcn_json_array_push(tools, tool);
    }

    lcn_json_set(result, "tools", tools);
    LcnJsonValue *resp = make_response(id, result);
    server_send(resp);
    lcn_json_free(resp);
}

static void handle_tools_call(LcnMcpServer *server, LcnJsonValue *id, LcnJsonValue *params) {
    const char *tool_name = lcn_json_get_string(params, "name");
    if (!tool_name) {
        LcnJsonValue *resp = make_error_response(id, -32602, "missing tool name");
        server_send(resp);
        lcn_json_free(resp);
        return;
    }

    /* Find handler */
    LcnMcpToolHandler handler = NULL;
    int i;
    for (i = 0; i < server->tool_count; i++) {
        if (strcmp(server->tools[i].name, tool_name) == 0) {
            handler = server->tools[i].handler;
            break;
        }
    }

    if (!handler) {
        LcnJsonValue *resp = make_error_response(id, -32601, "unknown tool");
        server_send(resp);
        lcn_json_free(resp);
        return;
    }

    /* Extract arguments as JSON string */
    LcnJsonValue *args_val = lcn_json_get(params, "arguments");
    char *args_str = args_val ? lcn_json_stringify(args_val) : NULL;
    if (!args_str) args_str = strdup("{}");

    /* Call handler */
    const char *result_text = handler(args_str);
    free(args_str);

    /* Build MCP content response */
    LcnJsonValue *result = lcn_json_object_new();
    LcnJsonValue *content = lcn_json_array_new();
    LcnJsonValue *item = lcn_json_object_new();
    lcn_json_set_string(item, "type", "text");
    lcn_json_set_string(item, "text", result_text ? result_text : "");
    lcn_json_array_push(content, item);
    lcn_json_set(result, "content", content);

    LcnJsonValue *resp = make_response(id, result);
    server_send(resp);
    lcn_json_free(resp);
}

/* ── Main server loop ────────────────────────────────────── */

int lcn_mcp_server_run(LcnMcpServer *server) {
    char *line;

    fprintf(stderr, "[mcp-server] %s v%s — listening on stdin\n",
            server->name, server->version);

    while ((line = server_read_line()) != NULL) {
        if (line[0] == '\0') { free(line); continue; }

        LcnJsonValue *msg = lcn_json_parse(line, strlen(line));
        free(line);
        if (!msg) continue;

        const char *method = lcn_json_get_string(msg, "method");
        LcnJsonValue *id = lcn_json_get(msg, "id");
        LcnJsonValue *params = lcn_json_get(msg, "params");

        if (!method) {
            lcn_json_free(msg);
            continue;
        }

        /* Notifications (no id) — just acknowledge silently */
        if (!id) {
            /* notifications/initialized, etc. */
            lcn_json_free(msg);
            continue;
        }

        if (strcmp(method, "initialize") == 0) {
            handle_initialize(server, id);
        } else if (strcmp(method, "tools/list") == 0) {
            handle_tools_list(server, id);
        } else if (strcmp(method, "tools/call") == 0) {
            handle_tools_call(server, id, params);
        } else {
            LcnJsonValue *resp = make_error_response(id, -32601, "method not found");
            server_send(resp);
            lcn_json_free(resp);
        }

        lcn_json_free(msg);
    }

    fprintf(stderr, "[mcp-server] %s — stdin closed, shutting down\n", server->name);
    return 0;
}
