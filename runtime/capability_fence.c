/* ============================================================
 * Limceron Runtime — Capability Fence (Defense-in-Depth)
 *
 * Runtime enforcement layer that validates capability-based access
 * control AFTER compile-time checks have already passed. This is
 * defense-in-depth: even if an attacker bypasses the type checker,
 * these runtime checks will catch unauthorized tool/resource access.
 *
 * Three enforcement points:
 *   1. Tool dispatch from LLM ToolCall responses
 *   2. MCP server tool invocation
 *   3. Resource access (endpoints, binaries, file paths)
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>

#include "capability_fence.h"

/* ── Forward declarations (matching lcn_runtime.h types) ── */

typedef struct {
    const char *host;
    int         port;
    bool        allow;
    const char *path_glob;
} LcnEndpointRule_CF;

typedef struct {
    const char *path;
    bool        allow;
} LcnBinaryRule_CF;

typedef struct {
    const char *pattern;
    bool        allow;
    bool        can_read;
    bool        can_write;
} LcnPathRule_CF;

typedef struct {
    const LcnEndpointRule_CF *endpoints;
    const LcnBinaryRule_CF   *binaries;
    const LcnPathRule_CF     *paths;
    bool                      deny_private;
    bool                      default_deny;
} LcnAccessPolicy_CF;

/* ── Internal helpers ── */

/* Case-insensitive string compare for tool names */
static bool cf_streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* Check if an IP address is in a private range */
static bool cf_is_private_ip(const char *host) {
    unsigned a, b, c, d;
    if (!host) return false;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 127) return true;
    if (a == 169 && b == 254) return true;
    if (a == 0) return true;
    (void)c; (void)d;
    return false;
}

/* Simple glob matching for path patterns */
static bool cf_glob_match(const char *pat, const char *str) {
    if (!pat || !str) return !pat && !str;
    while (*pat && *str) {
        if (pat[0] == '*' && pat[1] == '*') {
            pat += 2;
            if (*pat == '/') pat++;
            if (!*pat) return true;
            {
                const char *s;
                for (s = str; *s; s++) {
                    if (cf_glob_match(pat, s)) return true;
                }
            }
            return cf_glob_match(pat, str + strlen(str));
        }
        if (*pat == '*') {
            pat++;
            while (*str && *str != '/') {
                if (cf_glob_match(pat, str)) return true;
                str++;
            }
            return cf_glob_match(pat, str);
        }
        if (*pat != *str) return false;
        pat++; str++;
    }
    while (*pat == '*') pat++;
    return !*pat && !*str;
}

/* Parse host and port from a URL */
static bool cf_parse_url(const char *url, char *host, size_t host_sz,
                          int *port) {
    const char *p = url;
    const char *slash;
    const char *colon;

    if (!url || !host || !port) return false;

    host[0] = '\0';
    *port = 80;

    if (strncmp(p, "https://", 8) == 0) { *port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { *port = 80; p += 7; }
    else return false;

    slash = strchr(p, '/');
    colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_sz) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= host_sz) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    } else {
        size_t hlen = strlen(p);
        if (hlen >= host_sz) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    }

    return host[0] != '\0';
}

/* Extract base name from a path (last component after '/') */
static const char *cf_basename(const char *fpath) {
    const char *slash = strrchr(fpath, '/');
    return slash ? slash + 1 : fpath;
}

/* ── Public API ── */

bool lcn_capability_check_tool(const char *tool_name,
                                const char **allowed_tools,
                                int tool_count) {
    int i;

    if (!tool_name) return false;
    if (!allowed_tools || tool_count <= 0) return false;

    for (i = 0; i < tool_count; i++) {
        if (allowed_tools[i] && cf_streq(tool_name, allowed_tools[i])) {
            return true;
        }
    }

    return false;
}

bool lcn_capability_check_binary(const char *binary,
                                  const void *policy) {
    const LcnAccessPolicy_CF *p;
    const LcnBinaryRule_CF *r;
    char bin[256];
    size_t i;
    const char *src;
    const char *cmd_base;
    bool found;

    if (!binary) return false;
    if (!policy) return true; /* no policy = allow */

    p = (const LcnAccessPolicy_CF *)policy;

    /* Extract binary name (first word) */
    i = 0;
    src = binary;
    while (*src && !isspace((unsigned char)*src) && i < 255) {
        bin[i++] = *src++;
    }
    bin[i] = '\0';
    cmd_base = cf_basename(bin);

    /* Check explicit deny rules */
    if (p->binaries) {
        for (r = p->binaries; r->path; r++) {
            if (!r->allow) {
                const char *rule_base = cf_basename(r->path);
                if (strcmp(rule_base, cmd_base) == 0 ||
                    strcmp(r->path, bin) == 0) {
                    return false;
                }
            }
        }
    }

    /* Check allow rules */
    found = false;
    if (p->binaries) {
        for (r = p->binaries; r->path; r++) {
            if (r->allow) {
                const char *rule_base = cf_basename(r->path);
                if (strcmp(rule_base, cmd_base) == 0 ||
                    strcmp(r->path, bin) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found && p->default_deny) return false;

    return true;
}

bool lcn_capability_check_endpoint(const char *url,
                                    const void *policy) {
    const LcnAccessPolicy_CF *p;
    const LcnEndpointRule_CF *r;
    char host[256];
    int port;
    bool found;

    if (!url) return false;
    if (!policy) return true; /* no policy = allow */

    p = (const LcnAccessPolicy_CF *)policy;

    if (!cf_parse_url(url, host, sizeof(host), &port)) {
        return false; /* invalid URL = deny */
    }

    /* Check deny_private */
    if (p->deny_private && cf_is_private_ip(host)) {
        return false;
    }

    /* Check explicit deny rules */
    if (p->endpoints) {
        for (r = p->endpoints; r->host; r++) {
            if (!r->allow && strcmp(r->host, host) == 0 &&
                (r->port == 0 || r->port == port)) {
                return false;
            }
        }
    }

    /* Check allow rules */
    found = false;
    if (p->endpoints) {
        for (r = p->endpoints; r->host; r++) {
            if (r->allow && strcmp(r->host, host) == 0 &&
                (r->port == 0 || r->port == port)) {
                found = true;
                break;
            }
        }
    }

    if (!found && p->default_deny) return false;

    return true;
}

bool lcn_capability_check_path(const char *path, bool is_write,
                                const void *policy) {
    const LcnAccessPolicy_CF *p;
    const LcnPathRule_CF *r;
    bool found;

    if (!path) return false;
    if (!policy) return true; /* no policy = allow */

    p = (const LcnAccessPolicy_CF *)policy;
    if (!p->paths) return true;

    /* Check explicit deny rules first */
    for (r = p->paths; r->pattern; r++) {
        if (!r->allow && cf_glob_match(r->pattern, path)) {
            return false;
        }
    }

    /* Check allow rules */
    found = false;
    for (r = p->paths; r->pattern; r++) {
        if (r->allow && cf_glob_match(r->pattern, path)) {
            if (is_write && !r->can_write) return false;
            if (!is_write && !r->can_read) return false;
            found = true;
            break;
        }
    }

    if (!found && p->default_deny) return false;

    return true;
}

void lcn_capability_violation(const char *agent_name,
                               const char *tool_name,
                               const char *reason) {
    fprintf(stderr,
            "SECURITY: capability violation — agent '%s', tool '%s': %s\n",
            agent_name ? agent_name : "<unknown>",
            tool_name  ? tool_name  : "<unknown>",
            reason     ? reason     : "unspecified");
}
