#ifndef LCN_MCP_H
#define LCN_MCP_H

#include "json.h"
#include <stdbool.h>

/* MCP tool description */
typedef struct {
    char *name;
    char *description;
    char *input_schema;     /* JSON schema string */
} LcnMcpTool;

/* MCP connection (subprocess with pipes) */
typedef struct {
    int     stdin_fd;       /* write to child's stdin */
    int     stdout_fd;      /* read from child's stdout */
    int     pid;            /* child process PID */
    int     next_id;        /* JSON-RPC request ID counter */
    bool    initialized;    /* handshake complete */
} LcnMcpClient;

/* Connect to an MCP server by launching a subprocess.
 * command: executable path (e.g. "npx")
 * args: NULL-terminated array of arguments (e.g. {"@modelcontextprotocol/server-filesystem", "/tmp", NULL})
 * Returns NULL on error. */
LcnMcpClient *lcn_mcp_connect(const char *command, const char **args);

/* List available tools. Returns array of tools (caller must free with lcn_mcp_free_tools).
 * out_count receives the number of tools. */
LcnMcpTool *lcn_mcp_list_tools(LcnMcpClient *client, int *out_count);

/* Call a tool by name with JSON arguments string. Returns result as JSON value (caller must free). */
LcnJsonValue *lcn_mcp_call_tool(LcnMcpClient *client, const char *name, const char *args_json);

/* Close the MCP connection and kill the subprocess. */
void lcn_mcp_close(LcnMcpClient *client);

/* Free a tools array returned by lcn_mcp_list_tools. */
void lcn_mcp_free_tools(LcnMcpTool *tools, int count);

/* Close all cached MCP connections (used by lcn_mcp_dispatch).
 * Safe to call at program exit or when no more MCP calls are expected. */
void lcn_mcp_shutdown(void);

#endif /* LCN_MCP_H */
