/*
 * Limceron MCP (Model Context Protocol) Client
 *
 * Implements the MCP stdio transport: launches a subprocess, communicates
 * via JSON-RPC 2.0 over stdin/stdout pipes.
 *
 * Spec reference: https://spec.modelcontextprotocol.io/
 * Protocol version: 2024-11-05
 */

#include "mcp.h"
#include "json.h"
#include "event.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define MCP_READ_TIMEOUT_MS  30000   /* 30 seconds */
#define MCP_LINE_INITIAL_CAP 4096
#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_CLIENT_NAME      "limceron"
#define MCP_CLIENT_VERSION   "0.1.0"

/* --------------------------------------------------------------------------
 * Internal: line buffer for reading from the subprocess stdout
 * -------------------------------------------------------------------------- */

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
} McpLineBuffer;

static McpLineBuffer *mcp_line_buffer_new(void)
{
    McpLineBuffer *lb = calloc(1, sizeof(McpLineBuffer));
    if (!lb) {
        return NULL;
    }
    lb->cap = MCP_LINE_INITIAL_CAP;
    lb->buf = malloc(lb->cap);
    if (!lb->buf) {
        free(lb);
        return NULL;
    }
    lb->len = 0;
    return lb;
}

static void mcp_line_buffer_free(McpLineBuffer *lb)
{
    if (lb) {
        free(lb->buf);
        free(lb);
    }
}

static bool mcp_line_buffer_push(McpLineBuffer *lb, char c)
{
    if (lb->len + 1 >= lb->cap) {
        size_t new_cap = lb->cap * 2;
        char *new_buf = realloc(lb->buf, new_cap);
        if (!new_buf) {
            return false;
        }
        lb->buf = new_buf;
        lb->cap = new_cap;
    }
    lb->buf[lb->len++] = c;
    lb->buf[lb->len] = '\0';
    return true;
}

/* --------------------------------------------------------------------------
 * Internal: I/O helpers
 * -------------------------------------------------------------------------- */

/* Send a JSON value to the child's stdin as a single line (JSON + newline).
 * Returns true on success. */
static bool mcp_send(LcnMcpClient *client, LcnJsonValue *msg)
{
    char *str = lcn_json_stringify(msg);
    if (!str) {
        return false;
    }

    size_t slen = strlen(str);

    /* Allocate space for the string + newline */
    char *line = malloc(slen + 2);
    if (!line) {
        free(str);
        return false;
    }
    memcpy(line, str, slen);
    line[slen] = '\n';
    line[slen + 1] = '\0';
    free(str);

    size_t total = slen + 1;
    size_t written = 0;

    while (written < total) {
        ssize_t n = write(client->stdin_fd, line + written,
                          total - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(line);
            return false;
        }
        written += (size_t)n;
    }

    free(line);
    return true;
}

/* Read a single line from the child's stdout, with timeout.
 * Returns parsed JSON value, or NULL on error/timeout. */
static LcnJsonValue *mcp_recv(LcnMcpClient *client)
{
    McpLineBuffer *lb = mcp_line_buffer_new();
    if (!lb) {
        return NULL;
    }

    struct pollfd pfd;
    pfd.fd = client->stdout_fd;
    pfd.events = POLLIN;

    for (;;) {
        int ret = poll(&pfd, 1, MCP_READ_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            mcp_line_buffer_free(lb);
            return NULL;
        }
        if (ret == 0) {
            /* Timeout */
            mcp_line_buffer_free(lb);
            return NULL;
        }

        char c;
        ssize_t n = read(client->stdout_fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            mcp_line_buffer_free(lb);
            return NULL;
        }
        if (n == 0) {
            /* EOF — child closed stdout */
            mcp_line_buffer_free(lb);
            return NULL;
        }

        if (c == '\n') {
            break;
        }

        if (!mcp_line_buffer_push(lb, c)) {
            mcp_line_buffer_free(lb);
            return NULL;
        }
    }

    /* Skip empty lines (some servers emit blank lines) */
    if (lb->len == 0) {
        mcp_line_buffer_free(lb);
        return mcp_recv(client);   /* recurse to read next line */
    }

    LcnJsonValue *val = lcn_json_parse(lb->buf, lb->len);
    mcp_line_buffer_free(lb);
    return val;
}

/* --------------------------------------------------------------------------
 * Internal: JSON-RPC helpers
 * -------------------------------------------------------------------------- */

/* Build a JSON-RPC 2.0 request with an auto-incremented id. */
static LcnJsonValue *mcp_build_request(LcnMcpClient *client,
                                        const char *method,
                                        LcnJsonValue *params)
{
    LcnJsonValue *req = lcn_json_object_new();
    if (!req) {
        return NULL;
    }

    lcn_json_set_string(req, "jsonrpc", "2.0");
    lcn_json_set_number(req, "id", (double)client->next_id);
    client->next_id++;
    lcn_json_set_string(req, "method", method);

    if (params) {
        lcn_json_set(req, "params", params);
    } else {
        lcn_json_set(req, "params", lcn_json_object_new());
    }

    return req;
}

/* Build a JSON-RPC 2.0 notification (no id field). */
static LcnJsonValue *mcp_build_notification(const char *method)
{
    LcnJsonValue *notif = lcn_json_object_new();
    if (!notif) {
        return NULL;
    }

    lcn_json_set_string(notif, "jsonrpc", "2.0");
    lcn_json_set_string(notif, "method", method);

    return notif;
}

/* Send a request and wait for the matching response.
 * Skips over notifications received in between.
 * Returns the full response JSON (caller frees). */
static LcnJsonValue *mcp_request(LcnMcpClient *client,
                                  const char *method,
                                  LcnJsonValue *params)
{
    LcnJsonValue *req = mcp_build_request(client, method, params);
    if (!req) {
        return NULL;
    }

    /* Remember the id we sent so we can match the response */
    int expected_id = client->next_id - 1;

    if (!mcp_send(client, req)) {
        lcn_json_free(req);
        return NULL;
    }
    lcn_json_free(req);

    /* Read responses, skipping notifications until we get our reply */
    for (;;) {
        LcnJsonValue *resp = mcp_recv(client);
        if (!resp) {
            return NULL;
        }

        /* Check if this is the response to our request (has "id" field) */
        LcnJsonValue *id_val = lcn_json_get(resp, "id");
        if (id_val && id_val->type == LCN_JSON_NUMBER) {
            int resp_id = (int)id_val->data.number;
            if (resp_id == expected_id) {
                return resp;
            }
        }

        /* Not our response — might be a server notification. Discard. */
        lcn_json_free(resp);
    }
}

/* --------------------------------------------------------------------------
 * Internal: MCP handshake
 * -------------------------------------------------------------------------- */

static bool mcp_initialize(LcnMcpClient *client)
{
    /* Build initialize params */
    LcnJsonValue *params = lcn_json_object_new();
    if (!params) {
        return false;
    }

    lcn_json_set_string(params, "protocolVersion", MCP_PROTOCOL_VERSION);
    lcn_json_set(params, "capabilities", lcn_json_object_new());

    LcnJsonValue *client_info = lcn_json_object_new();
    if (!client_info) {
        lcn_json_free(params);
        return false;
    }
    lcn_json_set_string(client_info, "name", MCP_CLIENT_NAME);
    lcn_json_set_string(client_info, "version", MCP_CLIENT_VERSION);
    lcn_json_set(params, "clientInfo", client_info);

    /* Send initialize request */
    LcnJsonValue *resp = mcp_request(client, "initialize", params);
    if (!resp) {
        return false;
    }

    /* Check for error in response */
    LcnJsonValue *error = lcn_json_get(resp, "error");
    if (error && error->type == LCN_JSON_OBJECT) {
        lcn_json_free(resp);
        return false;
    }

    lcn_json_free(resp);

    /* Send initialized notification */
    LcnJsonValue *notif = mcp_build_notification("notifications/initialized");
    if (!notif) {
        return false;
    }

    bool ok = mcp_send(client, notif);
    lcn_json_free(notif);

    if (ok) {
        client->initialized = true;
    }
    return ok;
}

/* --------------------------------------------------------------------------
 * Internal: subprocess launch
 * -------------------------------------------------------------------------- */

/* Count the number of entries in a NULL-terminated string array. */
static int mcp_count_args(const char **args)
{
    int count = 0;
    if (args) {
        while (args[count] != NULL) {
            count++;
        }
    }
    return count;
}

/* Build the argv array for execvp: [command, args..., NULL] */
static char **mcp_build_argv(const char *command, const char **args)
{
    int nargs = mcp_count_args(args);
    char **argv = calloc((size_t)(nargs + 2), sizeof(char *));
    if (!argv) {
        return NULL;
    }

    argv[0] = strdup(command);
    if (!argv[0]) {
        free(argv);
        return NULL;
    }

    for (int i = 0; i < nargs; i++) {
        argv[i + 1] = strdup(args[i]);
        if (!argv[i + 1]) {
            for (int j = 0; j <= i; j++) {
                free(argv[j]);
            }
            free(argv);
            return NULL;
        }
    }
    argv[nargs + 1] = NULL;

    return argv;
}

static void mcp_free_argv(char **argv)
{
    if (argv) {
        for (int i = 0; argv[i] != NULL; i++) {
            free(argv[i]);
        }
        free(argv);
    }
}

/* --------------------------------------------------------------------------
 * Public API: lcn_mcp_connect
 * -------------------------------------------------------------------------- */

LcnMcpClient *lcn_mcp_connect(const char *command, const char **args)
{
    if (!command) {
        return NULL;
    }

    /* pipe_in:  parent writes to pipe_in[1], child reads from pipe_in[0]
     * pipe_out: child writes to pipe_out[1], parent reads from pipe_out[0] */
    int pipe_in[2];
    int pipe_out[2];

    if (pipe(pipe_in) < 0) {
        return NULL;
    }
    if (pipe(pipe_out) < 0) {
        close(pipe_in[0]);
        close(pipe_in[1]);
        return NULL;
    }

    char **argv = mcp_build_argv(command, args);
    if (!argv) {
        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);
        return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        /* Fork failed */
        mcp_free_argv(argv);
        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);
        return NULL;
    }

    if (pid == 0) {
        /* ---- Child process ---- */

        /* Redirect stdin to pipe_in read end */
        close(pipe_in[1]);
        if (dup2(pipe_in[0], STDIN_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_in[0]);

        /* Redirect stdout to pipe_out write end */
        close(pipe_out[0]);
        if (dup2(pipe_out[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_out[1]);

        /* Redirect stderr to /dev/null to avoid polluting our pipe */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        execvp(argv[0], argv);
        /* If exec fails, exit immediately */
        _exit(127);
    }

    /* ---- Parent process ---- */
    mcp_free_argv(argv);

    /* Close the child's ends of the pipes */
    close(pipe_in[0]);
    close(pipe_out[1]);

    LcnMcpClient *client = calloc(1, sizeof(LcnMcpClient));
    if (!client) {
        close(pipe_in[1]);
        close(pipe_out[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return NULL;
    }

    client->stdin_fd    = pipe_in[1];
    client->stdout_fd   = pipe_out[0];
    client->pid         = (int)pid;
    client->next_id     = 1;
    client->initialized = false;

    /* Perform the MCP handshake */
    if (!mcp_initialize(client)) {
        lcn_mcp_close(client);
        return NULL;
    }

    return client;
}

/* --------------------------------------------------------------------------
 * Public API: lcn_mcp_list_tools
 * -------------------------------------------------------------------------- */

LcnMcpTool *lcn_mcp_list_tools(LcnMcpClient *client, int *out_count)
{
    if (!client || !out_count) {
        return NULL;
    }
    *out_count = 0;

    if (!client->initialized) {
        return NULL;
    }

    /* Send tools/list request */
    LcnJsonValue *resp = mcp_request(client, "tools/list", NULL);
    if (!resp) {
        return NULL;
    }

    /* Check for error */
    LcnJsonValue *error = lcn_json_get(resp, "error");
    if (error && error->type == LCN_JSON_OBJECT) {
        lcn_json_free(resp);
        return NULL;
    }

    /* Extract result.tools array */
    LcnJsonValue *result = lcn_json_get(resp, "result");
    if (!result || result->type != LCN_JSON_OBJECT) {
        lcn_json_free(resp);
        return NULL;
    }

    LcnJsonValue *tools_arr = lcn_json_get(result, "tools");
    if (!tools_arr || tools_arr->type != LCN_JSON_ARRAY) {
        lcn_json_free(resp);
        return NULL;
    }

    size_t count = lcn_json_array_len(tools_arr);
    if (count == 0) {
        lcn_json_free(resp);
        *out_count = 0;
        return NULL;
    }

    LcnMcpTool *tools = calloc(count, sizeof(LcnMcpTool));
    if (!tools) {
        lcn_json_free(resp);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        LcnJsonValue *tool = lcn_json_array_get(tools_arr, i);
        if (!tool || tool->type != LCN_JSON_OBJECT) {
            continue;
        }

        const char *name = lcn_json_get_string(tool, "name");
        const char *desc = lcn_json_get_string(tool, "description");

        tools[i].name = name ? strdup(name) : NULL;
        tools[i].description = desc ? strdup(desc) : NULL;

        /* Serialize inputSchema back to a JSON string */
        LcnJsonValue *schema = lcn_json_get(tool, "inputSchema");
        if (schema) {
            tools[i].input_schema = lcn_json_stringify(schema);
        } else {
            tools[i].input_schema = NULL;
        }
    }

    *out_count = (int)count;
    lcn_json_free(resp);
    return tools;
}

/* --------------------------------------------------------------------------
 * Public API: lcn_mcp_call_tool
 * -------------------------------------------------------------------------- */

LcnJsonValue *lcn_mcp_call_tool(LcnMcpClient *client,
                                  const char *name,
                                  const char *args_json)
{
    if (!client || !name) {
        return NULL;
    }

    if (!client->initialized) {
        return NULL;
    }

    /* Build params object */
    LcnJsonValue *params = lcn_json_object_new();
    if (!params) {
        return NULL;
    }

    lcn_json_set_string(params, "name", name);

    /* Parse the arguments JSON string into a value */
    if (args_json && args_json[0] != '\0') {
        LcnJsonValue *arguments = lcn_json_parse(args_json,
                                                   strlen(args_json));
        if (arguments) {
            lcn_json_set(params, "arguments", arguments);
        } else {
            /* If parsing fails, send empty arguments */
            lcn_json_set(params, "arguments", lcn_json_object_new());
        }
    } else {
        lcn_json_set(params, "arguments", lcn_json_object_new());
    }

    /* Send tools/call request */
    lcn_emit_tool_call("", name, "mcp");
    LcnJsonValue *resp = mcp_request(client, "tools/call", params);
    if (!resp) {
        return NULL;
    }

    /* Check for protocol-level error */
    LcnJsonValue *error = lcn_json_get(resp, "error");
    if (error && error->type == LCN_JSON_OBJECT) {
        /* Return the error object so the caller can inspect it */
        LcnJsonValue *err_copy = lcn_json_object_new();
        if (err_copy) {
            const char *msg = lcn_json_get_string(error, "message");
            lcn_json_set_string(err_copy, "error",
                                 msg ? msg : "unknown error");

            LcnJsonValue *code_val = lcn_json_get(error, "code");
            if (code_val && code_val->type == LCN_JSON_NUMBER) {
                lcn_json_set_number(err_copy, "code",
                                     code_val->data.number);
            }
        }
        lcn_json_free(resp);
        lcn_emit_tool_result("", name, false);
        return err_copy;
    }

    /* Extract result.content */
    LcnJsonValue *result = lcn_json_get(resp, "result");
    if (!result || result->type != LCN_JSON_OBJECT) {
        lcn_json_free(resp);
        return NULL;
    }

    LcnJsonValue *content = lcn_json_get(result, "content");
    if (!content || content->type != LCN_JSON_ARRAY) {
        lcn_json_free(resp);
        return NULL;
    }

    /* Build a result value: collect all content entries into an array.
     * Each entry has "type" and "text" (for text content). We rebuild
     * the array so it is independent of the response tree. */
    size_t content_len = lcn_json_array_len(content);
    LcnJsonValue *result_arr = lcn_json_array_new();
    if (!result_arr) {
        lcn_json_free(resp);
        return NULL;
    }

    for (size_t i = 0; i < content_len; i++) {
        LcnJsonValue *entry = lcn_json_array_get(content, i);
        if (!entry || entry->type != LCN_JSON_OBJECT) {
            continue;
        }

        LcnJsonValue *item = lcn_json_object_new();
        if (!item) {
            continue;
        }

        const char *type = lcn_json_get_string(entry, "type");
        if (type) {
            lcn_json_set_string(item, "type", type);
        }

        const char *text = lcn_json_get_string(entry, "text");
        if (text) {
            lcn_json_set_string(item, "text", text);
        }

        /* Preserve other fields like "mimeType" for resource content */
        const char *mime = lcn_json_get_string(entry, "mimeType");
        if (mime) {
            lcn_json_set_string(item, "mimeType", mime);
        }

        /* Preserve "data" for binary/blob content */
        const char *data = lcn_json_get_string(entry, "data");
        if (data) {
            lcn_json_set_string(item, "data", data);
        }

        lcn_json_array_push(result_arr, item);
    }

    /* Also check for isError flag on the result */
    LcnJsonValue *is_error = lcn_json_get(result, "isError");
    if (is_error && is_error->type == LCN_JSON_BOOL &&
        is_error->data.boolean) {
        /* Wrap in an object that signals a tool-level error */
        LcnJsonValue *wrapper = lcn_json_object_new();
        if (wrapper) {
            lcn_json_set(wrapper, "content", result_arr);
            lcn_json_set(wrapper, "isError", lcn_json_bool_new(true));
            lcn_json_free(resp);
            lcn_emit_tool_result("", name, false);
            return wrapper;
        }
    }

    lcn_json_free(resp);
    lcn_emit_tool_result("", name, true);
    return result_arr;
}

/* --------------------------------------------------------------------------
 * Public API: lcn_mcp_close
 * -------------------------------------------------------------------------- */

void lcn_mcp_close(LcnMcpClient *client)
{
    if (!client) {
        return;
    }

    /* Close the pipes first — this signals EOF to the child */
    if (client->stdin_fd >= 0) {
        close(client->stdin_fd);
        client->stdin_fd = -1;
    }
    if (client->stdout_fd >= 0) {
        close(client->stdout_fd);
        client->stdout_fd = -1;
    }

    if (client->pid > 0) {
        int status;
        pid_t ret;

        /* Give the child a moment to exit gracefully */
        ret = waitpid(client->pid, &status, WNOHANG);
        if (ret == 0) {
            /* Child still running — send SIGTERM */
            kill(client->pid, SIGTERM);

            /* Wait up to 5 seconds */
            for (int i = 0; i < 50; i++) {
                ret = waitpid(client->pid, &status, WNOHANG);
                if (ret != 0) {
                    break;
                }
                usleep(100000);  /* 100ms */
            }

            /* If still alive, force kill */
            if (ret == 0) {
                kill(client->pid, SIGKILL);
                waitpid(client->pid, &status, 0);
            }
        }
    }

    free(client);
}

/* --------------------------------------------------------------------------
 * Public API: lcn_mcp_free_tools
 * -------------------------------------------------------------------------- */

void lcn_mcp_free_tools(LcnMcpTool *tools, int count)
{
    if (!tools) {
        return;
    }

    for (int i = 0; i < count; i++) {
        free(tools[i].name);
        free(tools[i].description);
        free(tools[i].input_schema);
    }

    free(tools);
}

/* --------------------------------------------------------------------------
 * Public API: lcn_mcp_dispatch
 * Convenience wrapper with connection caching.
 *
 * On first call for a given server_command, spawns the subprocess via
 * /bin/sh -c (so multi-word commands like "npx @mcp/server-fs /tmp" work),
 * performs the MCP handshake, and caches the connection.  Subsequent calls
 * with the same server_command reuse the cached connection.
 *
 * Uses LcnMcpResult from lcn_runtime.h.
 * -------------------------------------------------------------------------- */

#include "lcn_runtime.h"

#define MCP_DISPATCH_MAX_CONNS 16

typedef struct {
    char          *command;   /* key — the server_command string */
    LcnMcpClient *client;    /* cached connection */
} McpCacheEntry;

static McpCacheEntry g_mcp_cache[MCP_DISPATCH_MAX_CONNS];
static int           g_mcp_cache_count = 0;

/* Look up or create a cached connection for the given shell command.
 * Unlike lcn_mcp_connect() (which takes an executable + args array),
 * this always routes through "/bin/sh -c <command>" so that compound
 * command strings work out of the box. */
static LcnMcpClient *mcp_cache_get(const char *command)
{
    int i;

    /* Search cache for an existing, initialized connection */
    for (i = 0; i < g_mcp_cache_count; i++) {
        if (g_mcp_cache[i].command &&
            strcmp(g_mcp_cache[i].command, command) == 0) {
            if (g_mcp_cache[i].client &&
                g_mcp_cache[i].client->initialized) {
                return g_mcp_cache[i].client;
            }
            /* Entry exists but connection is dead — remove it */
            if (g_mcp_cache[i].client) {
                lcn_mcp_close(g_mcp_cache[i].client);
                g_mcp_cache[i].client = NULL;
            }
            break;
        }
    }

    /* Spawn via /bin/sh so shell metacharacters / multi-word args work */
    const char *args[] = { "-c", command, NULL };
    LcnMcpClient *client = lcn_mcp_connect("/bin/sh", args);
    if (!client) {
        return NULL;
    }

    /* Store in cache */
    if (i < g_mcp_cache_count) {
        /* Re-use the slot we found above */
        g_mcp_cache[i].client = client;
    } else if (g_mcp_cache_count < MCP_DISPATCH_MAX_CONNS) {
        g_mcp_cache[g_mcp_cache_count].command = strdup(command);
        g_mcp_cache[g_mcp_cache_count].client  = client;
        g_mcp_cache_count++;
    }
    /* If cache is full we still return the connection, it just won't be
     * cached for reuse (and the caller can't close it — acceptable for
     * the rare 17th unique server). */

    return client;
}

LcnMcpResult lcn_mcp_dispatch(const char *server_command, const char *tool_name,
                                 const char *args_json)
{
    LcnMcpResult out;
    LcnMcpClient *client;
    LcnJsonValue *result;
    char *json_str;

    memset(&out, 0, sizeof(out));

    if (!server_command || !tool_name) {
        out.ok = false;
        out.error = strdup("server_command and tool_name are required");
        return out;
    }

    fprintf(stderr, "[mcp] server: %s\n", server_command);
    fprintf(stderr, "[mcp] tool:   %s\n", tool_name);

    client = mcp_cache_get(server_command);
    if (!client) {
        out.ok = false;
        out.error = strdup("failed to connect to MCP server");
        return out;
    }

    result = lcn_mcp_call_tool(client, tool_name, args_json);
    if (!result) {
        out.ok = false;
        out.error = strdup("MCP tool call returned NULL");
        return out;
    }

    json_str = lcn_json_stringify(result);
    lcn_json_free(result);

    if (json_str) {
        out.result_json = json_str;
        out.ok = true;
        out.error = NULL;
        fprintf(stderr, "[mcp] tool call successful\n");
    } else {
        out.ok = false;
        out.error = strdup("failed to serialize MCP result");
    }

    return out;
}

/* --------------------------------------------------------------------------
 * Public API: lcn_mcp_shutdown
 * Closes all cached MCP connections.  Safe to call at program exit.
 * -------------------------------------------------------------------------- */

void lcn_mcp_shutdown(void)
{
    int i;
    for (i = 0; i < g_mcp_cache_count; i++) {
        if (g_mcp_cache[i].client) {
            lcn_mcp_close(g_mcp_cache[i].client);
            g_mcp_cache[i].client = NULL;
        }
        free(g_mcp_cache[i].command);
        g_mcp_cache[i].command = NULL;
    }
    g_mcp_cache_count = 0;
}
