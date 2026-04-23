/*
 * Limceron MCP Server — Expose agent methods as MCP tools
 * Reads JSON-RPC 2.0 from stdin, dispatches to handlers, writes to stdout.
 * This is the mirror of mcp.c (which is the MCP client).
 * Pure C99, POSIX.
 */
#ifndef LCN_MCP_SERVER_H
#define LCN_MCP_SERVER_H

#include <stdbool.h>

/* Tool handler: receives args JSON string, returns result text.
 * The returned string is owned by the handler (server will not free it). */
typedef const char *(*LcnMcpToolHandler)(const char *args_json);

/* Registered tool */
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema;  /* JSON schema string */
    LcnMcpToolHandler handler;
} LcnMcpServerTool;

/* Server state */
typedef struct {
    const char *name;
    const char *version;
    LcnMcpServerTool tools[32];
    int tool_count;
} LcnMcpServer;

/* Initialize server with name and version */
void lcn_mcp_server_init(LcnMcpServer *server, const char *name, const char *version);

/* Register a tool handler */
void lcn_mcp_server_add_tool(LcnMcpServer *server, const char *name,
                              const char *description, const char *input_schema,
                              LcnMcpToolHandler handler);

/* Run the server loop: read stdin, dispatch, write stdout.
 * Blocks until stdin is closed (EOF). Returns 0 on clean exit, 1 on error. */
int lcn_mcp_server_run(LcnMcpServer *server);

#endif /* LCN_MCP_SERVER_H */
