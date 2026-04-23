/* ============================================================
 * Limceron Runtime — Access Control Policy Enforcement
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

/* Forward declarations from lcn_runtime.h */
typedef const char *LcnString;
typedef struct { bool ok; void *value; LcnString error; } LcnResult;

typedef struct {
    const char *host;
    int         port;
    bool        allow;
    const char *path_glob;
} LcnEndpointRule;

typedef struct {
    const char *path;
    bool        allow;
} LcnBinaryRule;

typedef struct {
    const char *pattern;
    bool        allow;
    bool        can_read;
    bool        can_write;
} LcnPathRule;

typedef struct {
    const LcnEndpointRule *endpoints;
    const LcnBinaryRule   *binaries;
    const LcnPathRule     *paths;
    bool                   deny_private;
    bool                   default_deny;
} LcnAccessPolicy;

static bool acl_is_private_ip(const char *host) {
    unsigned a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 10) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 127) return true;
    if (a == 169 && b == 254) return true;
    if (a == 0) return true;
    (void)d;
    return false;
}

static bool acl_glob_match(const char *pat, const char *str) {
    if (!pat || !str) return !pat && !str;
    while (*pat && *str) {
        if (pat[0] == '*' && pat[1] == '*') {
            pat += 2;
            if (*pat == '/') pat++;
            /* ** matches everything from here */
            if (!*pat) return true;
            {
                const char *s;
                for (s = str; *s; s++) {
                    if (acl_glob_match(pat, s)) return true;
                }
            }
            return acl_glob_match(pat, str + strlen(str));
        }
        if (*pat == '*') {
            pat++;
            while (*str && *str != '/') {
                if (acl_glob_match(pat, str)) return true;
                str++;
            }
            return acl_glob_match(pat, str);
        }
        if (*pat != *str) return false;
        pat++; str++;
    }
    while (*pat == '*') pat++;
    return !*pat && !*str;
}

static bool acl_parse_url(const char *url, char *host, int *port, char *path) {
    const char *p = url;
    const char *slash;
    const char *colon;

    host[0] = '\0';
    *port = 80;
    path[0] = '/';
    path[1] = '\0';

    if (strncmp(p, "https://", 8) == 0) { *port = 443; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { *port = 80; p += 7; }
    else return false;

    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= 256) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
        if (slash) { strncpy(path, slash, 1024); path[1023] = '\0'; }
    } else if (slash) {
        size_t hlen = (size_t)(slash - p);
        if (hlen >= 256) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        strncpy(path, slash, 1024);
        path[1023] = '\0';
    } else {
        size_t hlen = strlen(p);
        if (hlen >= 256) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    }
    return host[0] != '\0';
}

LcnResult lcn_fetch_checked(const char *url, const LcnAccessPolicy *policy) {
    char host[256];
    int port;
    char path[1024];
    LcnResult err;
    const LcnEndpointRule *r;
    bool found;

    err.ok = false;
    err.value = NULL;
    err.error = NULL;

    if (!policy) {
        /* No policy — allow (backward compat) */
        extern LcnString lcn_http_get(const char *);
        LcnResult ok_result;
        ok_result.ok = true;
        ok_result.value = (void *)lcn_http_get(url);
        ok_result.error = NULL;
        return ok_result;
    }

    if (!acl_parse_url(url, host, &port, path)) {
        err.error = "access_control: invalid URL";
        fprintf(stderr, "SECURITY: invalid URL blocked: %s\n", url);
        return err;
    }

    if (policy->deny_private && acl_is_private_ip(host)) {
        err.error = "access_control: private IP range denied";
        fprintf(stderr, "SECURITY: fetch to private range blocked: %s\n", url);
        return err;
    }

    /* Check explicit deny rules */
    if (policy->endpoints) {
        for (r = policy->endpoints; r->host; r++) {
            if (!r->allow && strcmp(r->host, host) == 0 &&
                (r->port == 0 || r->port == port)) {
                err.error = "access_control: endpoint explicitly denied";
                fprintf(stderr, "SECURITY: fetch denied by policy: %s\n", url);
                return err;
            }
        }
    }

    /* Check allow rules */
    found = false;
    if (policy->endpoints) {
        for (r = policy->endpoints; r->host; r++) {
            if (r->allow && strcmp(r->host, host) == 0 &&
                (r->port == 0 || r->port == port)) {
                if (!r->path_glob || acl_glob_match(r->path_glob, path)) {
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found && policy->default_deny) {
        err.error = "access_control: endpoint not in allow list";
        fprintf(stderr, "SECURITY: fetch not in allow list: %s\n", url);
        return err;
    }

    {
        extern LcnString lcn_http_get(const char *);
        LcnResult ok_result;
        ok_result.ok = true;
        ok_result.value = (void *)lcn_http_get(url);
        ok_result.error = NULL;
        return ok_result;
    }
}

static const char *acl_basename(const char *fpath) {
    const char *slash = strrchr(fpath, '/');
    return slash ? slash + 1 : fpath;
}

LcnResult lcn_exec_checked(const char *command, const LcnAccessPolicy *policy) {
    LcnResult err;
    char bin[256];
    size_t i;
    const char *p;
    const char *cmd_base;
    const LcnBinaryRule *r;
    bool found;
    int rc;

    err.ok = false;
    err.value = NULL;
    err.error = NULL;

    if (!policy) {
        /* No policy — just run it */
        LcnResult run_result;
        rc = system(command);
        run_result.ok = (rc == 0);
        run_result.value = NULL;
        run_result.error = (rc != 0) ? "exec failed" : NULL;
        return run_result;
    }

    /* Extract binary name (first word) */
    i = 0;
    p = command;
    while (*p && !isspace((unsigned char)*p) && i < 255) {
        bin[i++] = *p++;
    }
    bin[i] = '\0';

    cmd_base = acl_basename(bin);

    /* Check explicit deny rules */
    if (policy->binaries) {
        for (r = policy->binaries; r->path; r++) {
            if (!r->allow) {
                const char *rule_base = acl_basename(r->path);
                if (strcmp(rule_base, cmd_base) == 0 || strcmp(r->path, bin) == 0) {
                    err.error = "access_control: binary explicitly denied";
                    fprintf(stderr, "SECURITY: exec denied by policy: %s\n", command);
                    return err;
                }
            }
        }
    }

    /* Check allow rules */
    found = false;
    if (policy->binaries) {
        for (r = policy->binaries; r->path; r++) {
            if (r->allow) {
                const char *rule_base = acl_basename(r->path);
                if (strcmp(rule_base, cmd_base) == 0 || strcmp(r->path, bin) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found && policy->default_deny) {
        err.error = "access_control: binary not in allow list";
        fprintf(stderr, "SECURITY: exec not in allow list: %s\n", command);
        return err;
    }

    rc = system(command);
    {
        LcnResult run_result;
        run_result.ok = (rc == 0);
        run_result.value = NULL;
        run_result.error = (rc != 0) ? "exec failed" : NULL;
        return run_result;
    }
}

/* ── Check a file path against path policies ── */

static bool acl_check_path(const LcnAccessPolicy *policy, const char *fpath,
                           bool need_write, const char **out_error) {
    const LcnPathRule *r;
    bool found;

    if (!policy || !policy->paths) return true;

    /* Check explicit deny rules first */
    for (r = policy->paths; r->pattern; r++) {
        if (!r->allow && acl_glob_match(r->pattern, fpath)) {
            *out_error = "access_control: path explicitly denied";
            fprintf(stderr, "SECURITY: file path denied by policy: %s\n",
                    fpath);
            return false;
        }
    }

    /* Check allow rules */
    found = false;
    for (r = policy->paths; r->pattern; r++) {
        if (r->allow && acl_glob_match(r->pattern, fpath)) {
            /* Check mode permission */
            if (need_write && !r->can_write) {
                *out_error = "access_control: path write not permitted";
                fprintf(stderr,
                        "SECURITY: write to %s blocked (no write mode)\n",
                        fpath);
                return false;
            }
            if (!need_write && !r->can_read) {
                *out_error = "access_control: path read not permitted";
                fprintf(stderr,
                        "SECURITY: read from %s blocked (no read mode)\n",
                        fpath);
                return false;
            }
            found = true;
            break;
        }
    }

    if (!found && policy->default_deny) {
        *out_error = "access_control: path not in allow list";
        fprintf(stderr, "SECURITY: file path not in allow list: %s\n", fpath);
        return false;
    }

    return true;
}

LcnString lcn_read_file_checked(const char *path,
                                const LcnAccessPolicy *policy) {
    const char *err_msg = NULL;
    extern LcnString lcn_read_file(const char *);

    if (!policy) return lcn_read_file(path);

    if (!acl_check_path(policy, path, false, &err_msg)) {
        fprintf(stderr, "SECURITY: read_file blocked: %s (%s)\n",
                path, err_msg ? err_msg : "denied");
        return "";
    }

    return lcn_read_file(path);
}

bool lcn_write_file_checked(const char *path, const char *content,
                            const LcnAccessPolicy *policy) {
    const char *err_msg = NULL;
    extern bool lcn_write_file(const char *, const char *);

    if (!policy) return lcn_write_file(path, content);

    if (!acl_check_path(policy, path, true, &err_msg)) {
        fprintf(stderr, "SECURITY: write_file blocked: %s (%s)\n",
                path, err_msg ? err_msg : "denied");
        return false;
    }

    return lcn_write_file(path, content);
}
