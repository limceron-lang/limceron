/*
 * Limceron Runtime — Capability Fence (Defense-in-Depth)
 *
 * Runtime enforcement of capability-based access control.
 * Even if compile-time checks pass, these runtime checks provide
 * a second layer of defense against capability violations.
 *
 * Used to validate:
 *   1. Tool calls from LLM ToolCall responses
 *   2. MCP tool dispatch
 *   3. Binary, endpoint, and file path access
 */
#ifndef LCN_CAPABILITY_FENCE_H
#define LCN_CAPABILITY_FENCE_H

#include <stdbool.h>

/* Forward declarations */
typedef struct LcnAccessPolicy LcnAccessPolicy_;

/* Check if a tool name is in the agent's allowed tool list.
 * Returns true if allowed, false if the tool is not in the list. */
bool lcn_capability_check_tool(const char *tool_name,
                                const char **allowed_tools, int tool_count);

/* Check if a binary path is allowed by the access policy.
 * Returns true if allowed. */
bool lcn_capability_check_binary(const char *binary,
                                  const void *policy);

/* Check if an endpoint URL is allowed by the access policy.
 * Returns true if allowed. */
bool lcn_capability_check_endpoint(const char *url,
                                    const void *policy);

/* Check if a file path is allowed by the access policy.
 * Returns true if allowed. */
bool lcn_capability_check_path(const char *path, bool is_write,
                                const void *policy);

/* Log a capability violation to stderr with structured format.
 * Does NOT abort — caller decides how to handle the violation. */
void lcn_capability_violation(const char *agent_name,
                               const char *tool_name,
                               const char *reason);

#endif /* LCN_CAPABILITY_FENCE_H */
