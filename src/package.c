/*
 * Limceron Package Manager — Implementation
 *
 * Simplified TOML parser, semver, dependency resolution, lock files.
 * Supports: exact versions, compatible (^), path deps, git deps.
 */

#include "package.h"
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>

/* ============================================================
 * Internal: Simplified TOML Parser
 *
 * Only handles what we need:
 *   [table]
 *   key = "string"
 *   key = { path = "...", git = "..." }
 *   key = ["a", "b"]
 * ============================================================ */

typedef enum {
    TOML_NONE,
    TOML_TABLE_PACKAGE,
    TOML_TABLE_DEPS,
    TOML_TABLE_DEV_DEPS,
    TOML_TABLE_UNKNOWN,
} TomlSection;

/* Skip whitespace (not newlines) */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Skip to end of line */
static const char *skip_line(const char *p) {
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    return p;
}

/* Parse a quoted string. Advances *pp past closing quote.
 * Writes into buf (up to bufsize). Returns true on success. */
static bool parse_quoted_string(const char **pp, char *buf, size_t bufsize) {
    const char *p = *pp;
    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && *p != '\n' && i < bufsize - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':  buf[i++] = '\n'; break;
            case 't':  buf[i++] = '\t'; break;
            case '\\': buf[i++] = '\\'; break;
            case '"':  buf[i++] = '"'; break;
            default:   buf[i++] = *p; break;
            }
        } else {
            buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';

    if (*p == '"') p++;
    *pp = p;
    return true;
}

/* Parse a bare key (unquoted identifier: alphanumeric, -, _). */
static bool parse_bare_key(const char **pp, char *buf, size_t bufsize) {
    const char *p = *pp;
    size_t i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '-' || *p == '_') && i < bufsize - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    *pp = p;
    return i > 0;
}

/* Parse an inline table: { path = "...", git = "...", rev = "..." }
 * Returns dep kind. */
static bool parse_inline_dep(const char **pp, LcnDependency *dep) {
    const char *p = *pp;
    if (*p != '{') return false;
    p++;

    dep->kind = LCN_DEP_REGISTRY;  /* default */

    while (*p && *p != '}') {
        p = skip_ws(p);
        if (*p == '}' || *p == '\n') break;

        char key[64];
        if (!parse_bare_key(&p, key, sizeof(key))) break;

        p = skip_ws(p);
        if (*p != '=') break;
        p++;
        p = skip_ws(p);

        char val[LCN_PKG_MAX_PATH];
        if (!parse_quoted_string(&p, val, sizeof(val))) break;

        if (strcmp(key, "path") == 0) {
            dep->kind = LCN_DEP_PATH;
            strncpy(dep->path, val, LCN_PKG_MAX_PATH - 1);
            dep->path[LCN_PKG_MAX_PATH - 1] = '\0';
        } else if (strcmp(key, "git") == 0) {
            dep->kind = LCN_DEP_GIT;
            strncpy(dep->git_url, val, LCN_PKG_MAX_PATH - 1);
            dep->git_url[LCN_PKG_MAX_PATH - 1] = '\0';
        } else if (strcmp(key, "rev") == 0 || strcmp(key, "tag") == 0 ||
                   strcmp(key, "branch") == 0) {
            strncpy(dep->git_rev, val, LCN_PKG_MAX_VERSION - 1);
            dep->git_rev[LCN_PKG_MAX_VERSION - 1] = '\0';
        } else if (strcmp(key, "version") == 0) {
            strncpy(dep->version, val, LCN_PKG_MAX_VERSION - 1);
            dep->version[LCN_PKG_MAX_VERSION - 1] = '\0';
        }

        p = skip_ws(p);
        if (*p == ',') p++;
    }

    if (*p == '}') p++;
    *pp = p;
    return true;
}

/* ============================================================
 * TOML Parser — Main
 * ============================================================ */

bool lcn_package_parse(const char *toml, size_t len, LcnPackage *out) {
    memset(out, 0, sizeof(LcnPackage));

    const char *p = toml;
    const char *end = toml + len;
    TomlSection section = TOML_NONE;

    while (p < end && *p) {
        /* Skip blank lines and comments */
        p = skip_ws(p);
        if (*p == '\n') { p++; continue; }
        if (*p == '#') { p = skip_line(p); continue; }
        if (*p == '\0') break;

        /* Table header: [section] */
        if (*p == '[') {
            p++;
            /* Check for [[array]] — skip for now */
            if (*p == '[') { p = skip_line(p); section = TOML_TABLE_UNKNOWN; continue; }

            char table_name[128];
            size_t ti = 0;
            while (*p && *p != ']' && *p != '\n' && ti < sizeof(table_name) - 1) {
                table_name[ti++] = *p++;
            }
            table_name[ti] = '\0';
            if (*p == ']') p++;

            /* Trim whitespace from table name */
            {
                char *s = table_name;
                while (*s == ' ') s++;
                char *e = table_name + ti - 1;
                while (e > s && *e == ' ') { *e = '\0'; e--; }
                /* Re-copy trimmed name */
                if (s != table_name) memmove(table_name, s, strlen(s) + 1);
            }

            if (strcmp(table_name, "package") == 0) {
                section = TOML_TABLE_PACKAGE;
            } else if (strcmp(table_name, "dependencies") == 0) {
                section = TOML_TABLE_DEPS;
            } else if (strcmp(table_name, "dev-dependencies") == 0) {
                section = TOML_TABLE_DEV_DEPS;
            } else {
                section = TOML_TABLE_UNKNOWN;
            }

            p = skip_line(p);
            continue;
        }

        /* Key = Value */
        char key[128];
        if (!parse_bare_key(&p, key, sizeof(key))) {
            p = skip_line(p);
            continue;
        }

        p = skip_ws(p);
        if (*p != '=') {
            p = skip_line(p);
            continue;
        }
        p++;
        p = skip_ws(p);

        if (section == TOML_TABLE_PACKAGE) {
            char val[LCN_PKG_MAX_DESC];
            if (*p == '"') {
                if (!parse_quoted_string(&p, val, sizeof(val))) {
                    p = skip_line(p);
                    continue;
                }
            } else {
                /* Unquoted value (shouldn't happen in valid TOML for strings) */
                size_t vi = 0;
                while (*p && *p != '\n' && *p != '#' && vi < sizeof(val) - 1) {
                    val[vi++] = *p++;
                }
                val[vi] = '\0';
                /* Trim trailing whitespace */
                while (vi > 0 && (val[vi-1] == ' ' || val[vi-1] == '\t')) {
                    val[--vi] = '\0';
                }
            }

            if (strcmp(key, "name") == 0) {
                strncpy(out->name, val, LCN_PKG_MAX_NAME - 1);
            } else if (strcmp(key, "version") == 0) {
                strncpy(out->version, val, LCN_PKG_MAX_VERSION - 1);
            } else if (strcmp(key, "description") == 0) {
                strncpy(out->description, val, LCN_PKG_MAX_DESC - 1);
            }
        }
        else if (section == TOML_TABLE_DEPS || section == TOML_TABLE_DEV_DEPS) {
            if (out->dep_count >= LCN_PKG_MAX_DEPS) {
                p = skip_line(p);
                continue;
            }

            LcnDependency *dep = &out->deps[out->dep_count];
            memset(dep, 0, sizeof(LcnDependency));
            strncpy(dep->name, key, LCN_PKG_MAX_NAME - 1);
            dep->is_dev = (section == TOML_TABLE_DEV_DEPS);

            if (*p == '"') {
                /* Simple version string */
                char ver[LCN_PKG_MAX_VERSION];
                if (parse_quoted_string(&p, ver, sizeof(ver))) {
                    strncpy(dep->version, ver, LCN_PKG_MAX_VERSION - 1);
                    dep->kind = LCN_DEP_REGISTRY;
                }
            } else if (*p == '{') {
                /* Inline table */
                parse_inline_dep(&p, dep);
            }

            out->dep_count++;
        }

        p = skip_line(p);
    }

    return out->name[0] != '\0';
}

/* ============================================================
 * TOML Serializer
 * ============================================================ */

int lcn_package_serialize(const LcnPackage *pkg, char *buf, size_t bufsize) {
    int n = 0;

    n += snprintf(buf + n, bufsize - (size_t)n,
                  "[package]\n"
                  "name = \"%s\"\n"
                  "version = \"%s\"\n",
                  pkg->name, pkg->version);

    if (pkg->description[0]) {
        n += snprintf(buf + n, bufsize - (size_t)n,
                      "description = \"%s\"\n", pkg->description);
    }

    /* Regular dependencies */
    bool has_deps = false;
    for (int i = 0; i < pkg->dep_count; i++) {
        if (!pkg->deps[i].is_dev) { has_deps = true; break; }
    }

    if (has_deps) {
        n += snprintf(buf + n, bufsize - (size_t)n, "\n[dependencies]\n");
        for (int i = 0; i < pkg->dep_count; i++) {
            const LcnDependency *d = &pkg->deps[i];
            if (d->is_dev) continue;

            switch (d->kind) {
            case LCN_DEP_REGISTRY:
                n += snprintf(buf + n, bufsize - (size_t)n,
                              "%s = \"%s\"\n", d->name, d->version);
                break;
            case LCN_DEP_PATH:
                n += snprintf(buf + n, bufsize - (size_t)n,
                              "%s = { path = \"%s\" }\n", d->name, d->path);
                break;
            case LCN_DEP_GIT:
                if (d->git_rev[0]) {
                    n += snprintf(buf + n, bufsize - (size_t)n,
                                  "%s = { git = \"%s\", rev = \"%s\" }\n",
                                  d->name, d->git_url, d->git_rev);
                } else {
                    n += snprintf(buf + n, bufsize - (size_t)n,
                                  "%s = { git = \"%s\" }\n", d->name, d->git_url);
                }
                break;
            }
        }
    }

    /* Dev dependencies */
    bool has_dev_deps = false;
    for (int i = 0; i < pkg->dep_count; i++) {
        if (pkg->deps[i].is_dev) { has_dev_deps = true; break; }
    }

    if (has_dev_deps) {
        n += snprintf(buf + n, bufsize - (size_t)n, "\n[dev-dependencies]\n");
        for (int i = 0; i < pkg->dep_count; i++) {
            const LcnDependency *d = &pkg->deps[i];
            if (!d->is_dev) continue;

            switch (d->kind) {
            case LCN_DEP_REGISTRY:
                n += snprintf(buf + n, bufsize - (size_t)n,
                              "%s = \"%s\"\n", d->name, d->version);
                break;
            case LCN_DEP_PATH:
                n += snprintf(buf + n, bufsize - (size_t)n,
                              "%s = { path = \"%s\" }\n", d->name, d->path);
                break;
            case LCN_DEP_GIT:
                if (d->git_rev[0]) {
                    n += snprintf(buf + n, bufsize - (size_t)n,
                                  "%s = { git = \"%s\", rev = \"%s\" }\n",
                                  d->name, d->git_url, d->git_rev);
                } else {
                    n += snprintf(buf + n, bufsize - (size_t)n,
                                  "%s = { git = \"%s\" }\n", d->name, d->git_url);
                }
                break;
            }
        }
    }

    return n;
}

/* ============================================================
 * Package File I/O
 * ============================================================ */

bool lcn_package_load(const char *dir, LcnPackage *out) {
    char path[LCN_PKG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/limceron.toml", dir);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return false;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return false; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    bool ok = lcn_package_parse(buf, read, out);
    free(buf);
    return ok;
}

bool lcn_package_save(const char *dir, const LcnPackage *pkg) {
    char path[LCN_PKG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/limceron.toml", dir);

    char buf[8192];
    int n = lcn_package_serialize(pkg, buf, sizeof(buf));
    if (n <= 0) return false;

    FILE *f = fopen(path, "w");
    if (!f) return false;

    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    return true;
}

/* ============================================================
 * Package Mutation
 * ============================================================ */

bool lcn_package_add_dep(LcnPackage *pkg, const char *name,
                         const char *version, LcnDepKind kind,
                         const char *path_or_url, bool is_dev) {
    if (pkg->dep_count >= LCN_PKG_MAX_DEPS) return false;

    /* Check for existing dep with same name — update in place */
    for (int i = 0; i < pkg->dep_count; i++) {
        if (strcmp(pkg->deps[i].name, name) == 0) {
            LcnDependency *d = &pkg->deps[i];
            if (version) strncpy(d->version, version, LCN_PKG_MAX_VERSION - 1);
            d->kind = kind;
            d->is_dev = is_dev;
            if (path_or_url) {
                if (kind == LCN_DEP_PATH)
                    strncpy(d->path, path_or_url, LCN_PKG_MAX_PATH - 1);
                else if (kind == LCN_DEP_GIT)
                    strncpy(d->git_url, path_or_url, LCN_PKG_MAX_PATH - 1);
            }
            return true;
        }
    }

    LcnDependency *d = &pkg->deps[pkg->dep_count];
    memset(d, 0, sizeof(LcnDependency));
    strncpy(d->name, name, LCN_PKG_MAX_NAME - 1);
    if (version) strncpy(d->version, version, LCN_PKG_MAX_VERSION - 1);
    d->kind = kind;
    d->is_dev = is_dev;
    if (path_or_url) {
        if (kind == LCN_DEP_PATH)
            strncpy(d->path, path_or_url, LCN_PKG_MAX_PATH - 1);
        else if (kind == LCN_DEP_GIT)
            strncpy(d->git_url, path_or_url, LCN_PKG_MAX_PATH - 1);
    }
    pkg->dep_count++;
    return true;
}

bool lcn_package_remove_dep(LcnPackage *pkg, const char *name) {
    for (int i = 0; i < pkg->dep_count; i++) {
        if (strcmp(pkg->deps[i].name, name) == 0) {
            /* Shift remaining deps down */
            for (int j = i; j < pkg->dep_count - 1; j++) {
                pkg->deps[j] = pkg->deps[j + 1];
            }
            pkg->dep_count--;
            return true;
        }
    }
    return false;
}

/* ============================================================
 * Semver
 * ============================================================ */

bool semver_parse(const char *str, LcnSemver *out) {
    if (!str || !out) return false;
    out->major = out->minor = out->patch = 0;

    /* Skip leading 'v' if present */
    if (*str == 'v' || *str == 'V') str++;

    int fields = sscanf(str, "%d.%d.%d", &out->major, &out->minor, &out->patch);
    return fields >= 1;
}

int semver_compare(const char *a, const char *b) {
    LcnSemver sa, sb;
    if (!semver_parse(a, &sa)) return -1;
    if (!semver_parse(b, &sb)) return 1;

    if (sa.major != sb.major) return sa.major - sb.major;
    if (sa.minor != sb.minor) return sa.minor - sb.minor;
    return sa.patch - sb.patch;
}

bool semver_satisfies(const char *version, const char *constraint) {
    if (!version || !constraint) return false;
    if (strcmp(constraint, "*") == 0) return true;

    LcnSemver ver;
    if (!semver_parse(version, &ver)) return false;

    /* Compatible: ^X.Y.Z means >=X.Y.Z and <(X+1).0.0 */
    if (constraint[0] == '^') {
        LcnSemver con;
        if (!semver_parse(constraint + 1, &con)) return false;

        /* Must be >= constraint */
        if (ver.major < con.major) return false;
        if (ver.major > con.major) return false;  /* >= (major+1).0.0 */
        if (ver.minor < con.minor) return false;
        if (ver.minor == con.minor && ver.patch < con.patch) return false;
        return true;
    }

    /* Tilde: ~X.Y.Z means >=X.Y.Z and <X.(Y+1).0 */
    if (constraint[0] == '~') {
        LcnSemver con;
        if (!semver_parse(constraint + 1, &con)) return false;

        if (ver.major != con.major) return false;
        if (ver.minor != con.minor) return false;
        return ver.patch >= con.patch;
    }

    /* Prefix >=, >, <=, < */
    if (constraint[0] == '>' && constraint[1] == '=') {
        return semver_compare(version, constraint + 2) >= 0;
    }
    if (constraint[0] == '<' && constraint[1] == '=') {
        return semver_compare(version, constraint + 2) <= 0;
    }
    if (constraint[0] == '>') {
        return semver_compare(version, constraint + 1) > 0;
    }
    if (constraint[0] == '<') {
        return semver_compare(version, constraint + 1) < 0;
    }

    /* Exact match */
    return semver_compare(version, constraint) == 0;
}

/* ============================================================
 * Dependency Resolution
 * ============================================================ */

/* Simple SHA-256-ish hash for content verification.
 * This is a lightweight FNV-1a based hash formatted as hex —
 * not cryptographic, but sufficient for change detection. */
static void simple_hash_dir(const char *dir_path, char *out, size_t out_size) {
    /* Walk directory and hash file names + sizes */
    uint64_t h = 0xcbf29ce484222325ULL;  /* FNV offset basis */
    DIR *d = opendir(dir_path);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            for (int i = 0; ent->d_name[i]; i++) {
                h ^= (uint64_t)(unsigned char)ent->d_name[i];
                h *= 0x100000001b3ULL;
            }
            char full[LCN_PKG_MAX_PATH];
            snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
            struct stat st;
            if (stat(full, &st) == 0) {
                h ^= (uint64_t)st.st_size;
                h *= 0x100000001b3ULL;
            }
        }
        closedir(d);
    }
    snprintf(out, out_size, "fnv1a:%016llx", (unsigned long long)h);
}

static bool resolve_path_dep(const LcnDependency *dep, const char *project_dir,
                             LcnResolvedDep *out) {
    char abs_path[LCN_PKG_MAX_PATH];

    if (dep->path[0] == '/') {
        /* Absolute path */
        strncpy(abs_path, dep->path, sizeof(abs_path) - 1);
    } else {
        /* Relative to project dir */
        snprintf(abs_path, sizeof(abs_path), "%s/%s", project_dir, dep->path);
    }

    /* Verify directory exists */
    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "  package: path dependency '%s' not found at %s\n",
                dep->name, abs_path);
        return false;
    }

    strncpy(out->name, dep->name, LCN_PKG_MAX_NAME - 1);
    strncpy(out->path, abs_path, LCN_PKG_MAX_PATH - 1);

    /* Try to read version from dependency's limceron.toml */
    LcnPackage dep_pkg;
    if (lcn_package_load(abs_path, &dep_pkg) && dep_pkg.version[0]) {
        strncpy(out->resolved_version, dep_pkg.version, LCN_PKG_MAX_VERSION - 1);
    } else {
        strncpy(out->resolved_version, "0.0.0-local", LCN_PKG_MAX_VERSION - 1);
    }

    simple_hash_dir(abs_path, out->hash, sizeof(out->hash));
    return true;
}

static bool resolve_git_dep(const LcnDependency *dep, const char *project_dir,
                            LcnResolvedDep *out) {
    char pkg_dir[LCN_PKG_MAX_PATH];
    snprintf(pkg_dir, sizeof(pkg_dir), "%s/.limceron/packages/%s",
             project_dir, dep->name);

    /* Check if already cloned */
    struct stat st;
    if (stat(pkg_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        /* Already exists — use it */
        strncpy(out->name, dep->name, LCN_PKG_MAX_NAME - 1);
        strncpy(out->path, pkg_dir, LCN_PKG_MAX_PATH - 1);

        LcnPackage dep_pkg;
        if (lcn_package_load(pkg_dir, &dep_pkg) && dep_pkg.version[0]) {
            strncpy(out->resolved_version, dep_pkg.version, LCN_PKG_MAX_VERSION - 1);
        } else {
            strncpy(out->resolved_version, "0.0.0-git", LCN_PKG_MAX_VERSION - 1);
        }

        simple_hash_dir(pkg_dir, out->hash, sizeof(out->hash));
        return true;
    }

    /* Clone */
    char cmd[2048];
    char parent_dir[LCN_PKG_MAX_PATH];
    snprintf(parent_dir, sizeof(parent_dir), "%s/.limceron/packages", project_dir);

    /* Ensure parent directories exist */
    {
        char mkdir_cmd[LCN_PKG_MAX_PATH + 16];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", parent_dir);
        system(mkdir_cmd);
    }

    if (dep->git_rev[0]) {
        snprintf(cmd, sizeof(cmd),
                 "git clone --depth 1 --branch \"%s\" \"%s\" \"%s\" 2>&1",
                 dep->git_rev, dep->git_url, pkg_dir);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "git clone --depth 1 \"%s\" \"%s\" 2>&1",
                 dep->git_url, pkg_dir);
    }

    fprintf(stderr, "  package: cloning %s ...\n", dep->name);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  package: failed to clone '%s' from %s\n",
                dep->name, dep->git_url);
        return false;
    }

    strncpy(out->name, dep->name, LCN_PKG_MAX_NAME - 1);
    strncpy(out->path, pkg_dir, LCN_PKG_MAX_PATH - 1);

    LcnPackage dep_pkg;
    if (lcn_package_load(pkg_dir, &dep_pkg) && dep_pkg.version[0]) {
        strncpy(out->resolved_version, dep_pkg.version, LCN_PKG_MAX_VERSION - 1);
    } else {
        strncpy(out->resolved_version, "0.0.0-git", LCN_PKG_MAX_VERSION - 1);
    }

    simple_hash_dir(pkg_dir, out->hash, sizeof(out->hash));
    return true;
}

int lcn_resolve_dependencies(const LcnPackage *pkg, const char *project_dir,
                             LcnResolvedDep *out, int max) {
    int count = 0;

    for (int i = 0; i < pkg->dep_count && count < max; i++) {
        const LcnDependency *dep = &pkg->deps[i];
        LcnResolvedDep *resolved = &out[count];
        memset(resolved, 0, sizeof(LcnResolvedDep));

        bool ok = false;
        switch (dep->kind) {
        case LCN_DEP_PATH:
            ok = resolve_path_dep(dep, project_dir, resolved);
            break;
        case LCN_DEP_GIT:
            ok = resolve_git_dep(dep, project_dir, resolved);
            break;
        case LCN_DEP_REGISTRY:
            /* Registry deps resolve to .limceron/packages/ if already installed,
             * otherwise they need to be installed first. */
            {
                char pkg_dir[LCN_PKG_MAX_PATH];
                snprintf(pkg_dir, sizeof(pkg_dir), "%s/.limceron/packages/%s-%s",
                         project_dir, dep->name, dep->version);
                struct stat st;
                if (stat(pkg_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                    strncpy(resolved->name, dep->name, LCN_PKG_MAX_NAME - 1);
                    strncpy(resolved->path, pkg_dir, LCN_PKG_MAX_PATH - 1);
                    strncpy(resolved->resolved_version, dep->version,
                            LCN_PKG_MAX_VERSION - 1);
                    simple_hash_dir(pkg_dir, resolved->hash, sizeof(resolved->hash));
                    ok = true;
                } else {
                    /* Also try without version suffix */
                    snprintf(pkg_dir, sizeof(pkg_dir), "%s/.limceron/packages/%s",
                             project_dir, dep->name);
                    if (stat(pkg_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
                        strncpy(resolved->name, dep->name, LCN_PKG_MAX_NAME - 1);
                        strncpy(resolved->path, pkg_dir, LCN_PKG_MAX_PATH - 1);
                        strncpy(resolved->resolved_version, dep->version,
                                LCN_PKG_MAX_VERSION - 1);
                        simple_hash_dir(pkg_dir, resolved->hash, sizeof(resolved->hash));
                        ok = true;
                    } else {
                        fprintf(stderr, "  package: '%s' %s not installed "
                                "(run `limceron install`)\n",
                                dep->name, dep->version);
                    }
                }
            }
            break;
        }

        if (ok) count++;
    }

    return count;
}

/* ============================================================
 * Lock File
 * ============================================================ */

bool lcn_lock_parse(const char *content, size_t len, LcnLockFile *out) {
    memset(out, 0, sizeof(LcnLockFile));

    const char *p = content;
    const char *end = content + len;

    while (p < end && *p) {
        /* Skip comments and blank lines */
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || *p == '\0') break;
        if (*p == '#' || *p == '\n') {
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            continue;
        }

        if (out->count >= LCN_PKG_MAX_LOCK_ENTRIES) break;

        LcnResolvedDep *entry = &out->entries[out->count];
        memset(entry, 0, sizeof(LcnResolvedDep));

        /* Parse: name version hash */
        /* Name */
        {
            size_t i = 0;
            while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
                   i < LCN_PKG_MAX_NAME - 1) {
                entry->name[i++] = *p++;
            }
            entry->name[i] = '\0';
        }

        while (p < end && (*p == ' ' || *p == '\t')) p++;

        /* Version */
        {
            size_t i = 0;
            while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
                   i < LCN_PKG_MAX_VERSION - 1) {
                entry->resolved_version[i++] = *p++;
            }
            entry->resolved_version[i] = '\0';
        }

        while (p < end && (*p == ' ' || *p == '\t')) p++;

        /* Hash */
        {
            size_t i = 0;
            while (p < end && *p != ' ' && *p != '\t' && *p != '\n' &&
                   i < LCN_PKG_HASH_LEN - 1) {
                entry->hash[i++] = *p++;
            }
            entry->hash[i] = '\0';
        }

        /* Skip to next line */
        while (p < end && *p != '\n') p++;
        if (p < end) p++;

        if (entry->name[0]) out->count++;
    }

    return true;
}

int lcn_lock_serialize(const LcnLockFile *lock, char *buf, size_t bufsize) {
    int n = 0;
    n += snprintf(buf + n, bufsize - (size_t)n,
                  "# limceron.lock - auto-generated, do not edit\n");

    for (int i = 0; i < lock->count; i++) {
        const LcnResolvedDep *e = &lock->entries[i];
        n += snprintf(buf + n, bufsize - (size_t)n,
                      "%s %s %s\n",
                      e->name, e->resolved_version,
                      e->hash[0] ? e->hash : "none");
    }

    return n;
}

bool lcn_lock_load(const char *dir, LcnLockFile *out) {
    char path[LCN_PKG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/limceron.lock", dir);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {
        fclose(f);
        return false;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return false; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';

    bool ok = lcn_lock_parse(buf, read, out);
    free(buf);
    return ok;
}

bool lcn_lock_save(const char *dir, const LcnLockFile *lock) {
    char path[LCN_PKG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/limceron.lock", dir);

    char buf[16384];
    int n = lcn_lock_serialize(lock, buf, sizeof(buf));
    if (n <= 0) return false;

    FILE *f = fopen(path, "w");
    if (!f) return false;

    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    return true;
}

/* ============================================================
 * Package Installation
 * ============================================================ */

static void ensure_dir(const char *path) {
    char cmd[LCN_PKG_MAX_PATH + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", path);
    system(cmd);
}

bool lcn_install_dep(const char *project_dir, const LcnDependency *dep) {
    char pkg_base[LCN_PKG_MAX_PATH];
    snprintf(pkg_base, sizeof(pkg_base), "%s/.limceron/packages", project_dir);
    ensure_dir(pkg_base);

    char cache_dir[LCN_PKG_MAX_PATH];
    snprintf(cache_dir, sizeof(cache_dir), "%s/.limceron/cache", project_dir);
    ensure_dir(cache_dir);

    switch (dep->kind) {
    case LCN_DEP_PATH: {
        /* Path dependencies: symlink or copy to packages dir */
        char src[LCN_PKG_MAX_PATH], dst[LCN_PKG_MAX_PATH];
        if (dep->path[0] == '/') {
            strncpy(src, dep->path, sizeof(src) - 1);
        } else {
            snprintf(src, sizeof(src), "%s/%s", project_dir, dep->path);
        }
        snprintf(dst, sizeof(dst), "%s/%s", pkg_base, dep->name);

        /* Remove existing */
        {
            char rm_cmd[LCN_PKG_MAX_PATH + 16];
            snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", dst);
            system(rm_cmd);
        }

        /* Create symlink */
        char real_src[LCN_PKG_MAX_PATH];
        if (realpath(src, real_src)) {
            if (symlink(real_src, dst) == 0) {
                fprintf(stderr, "  installed: %s -> %s (path)\n", dep->name, real_src);
                return true;
            }
        }

        /* Fallback: copy */
        {
            char cp_cmd[2 * LCN_PKG_MAX_PATH + 32];
            snprintf(cp_cmd, sizeof(cp_cmd), "cp -r \"%s\" \"%s\"", src, dst);
            if (system(cp_cmd) == 0) {
                fprintf(stderr, "  installed: %s (copied from %s)\n", dep->name, src);
                return true;
            }
        }

        fprintf(stderr, "  error: cannot install path dep '%s' from %s\n",
                dep->name, dep->path);
        return false;
    }

    case LCN_DEP_GIT: {
        char dst[LCN_PKG_MAX_PATH];
        snprintf(dst, sizeof(dst), "%s/%s", pkg_base, dep->name);

        /* Check if already cloned */
        struct stat st;
        if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Pull latest */
            char cmd[LCN_PKG_MAX_PATH + 32];
            snprintf(cmd, sizeof(cmd), "cd \"%s\" && git pull --quiet 2>&1", dst);
            system(cmd);
            fprintf(stderr, "  updated: %s (git)\n", dep->name);
            return true;
        }

        /* Clone */
        char cmd[2048];
        if (dep->git_rev[0]) {
            snprintf(cmd, sizeof(cmd),
                     "git clone --depth 1 --branch \"%s\" \"%s\" \"%s\" 2>&1",
                     dep->git_rev, dep->git_url, dst);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "git clone --depth 1 \"%s\" \"%s\" 2>&1",
                     dep->git_url, dst);
        }

        fprintf(stderr, "  cloning: %s from %s ...\n", dep->name, dep->git_url);
        int rc = system(cmd);
        if (rc == 0) {
            fprintf(stderr, "  installed: %s (git)\n", dep->name);
            return true;
        }

        fprintf(stderr, "  error: cannot clone '%s' from %s\n",
                dep->name, dep->git_url);
        return false;
    }

    case LCN_DEP_REGISTRY:
        /* No registry server yet — just report */
        fprintf(stderr, "  skip: %s %s (no registry — use path or git deps)\n",
                dep->name, dep->version);
        return false;
    }

    return false;
}

int lcn_install_all(const char *project_dir, const LcnPackage *pkg) {
    int installed = 0;

    for (int i = 0; i < pkg->dep_count; i++) {
        if (lcn_install_dep(project_dir, &pkg->deps[i])) {
            installed++;
        }
    }

    /* Generate lock file */
    LcnResolvedDep resolved[LCN_PKG_MAX_DEPS];
    int resolved_count = lcn_resolve_dependencies(pkg, project_dir,
                                                   resolved, LCN_PKG_MAX_DEPS);

    if (resolved_count > 0) {
        LcnLockFile lock;
        memset(&lock, 0, sizeof(lock));
        for (int i = 0; i < resolved_count && i < LCN_PKG_MAX_LOCK_ENTRIES; i++) {
            lock.entries[lock.count++] = resolved[i];
        }
        lcn_lock_save(project_dir, &lock);
        fprintf(stderr, "  lock: wrote limceron.lock (%d entries)\n", lock.count);
    }

    return installed;
}

/* ============================================================
 * Package Publishing
 * ============================================================ */

bool lcn_publish_package(const char *dir, char *out_path, size_t out_path_size) {
    LcnPackage pkg;
    if (!lcn_package_load(dir, &pkg)) {
        fprintf(stderr, "error: no limceron.toml found in %s\n", dir);
        return false;
    }

    /* Build tarball name */
    char tarball[LCN_PKG_MAX_PATH];
    snprintf(tarball, sizeof(tarball), "%s-%s.tar.gz", pkg.name, pkg.version);

    /* Create tarball containing src/ and limceron.toml */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "cd \"%s\" && tar czf \"%s\" limceron.toml src/ 2>&1",
             dir, tarball);

    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "error: failed to create package tarball\n");
        return false;
    }

    char full_tarball[LCN_PKG_MAX_PATH];
    snprintf(full_tarball, sizeof(full_tarball), "%s/%s", dir, tarball);

    if (out_path && out_path_size > 0) {
        strncpy(out_path, full_tarball, out_path_size - 1);
        out_path[out_path_size - 1] = '\0';
    }

    return true;
}

/* ============================================================
 * Build Integration
 * ============================================================ */

int lcn_dep_import_paths(const char *project_dir, const LcnPackage *pkg,
                         char paths[][LCN_PKG_MAX_PATH], int max_paths) {
    LcnResolvedDep resolved[LCN_PKG_MAX_DEPS];
    int count = lcn_resolve_dependencies(pkg, project_dir,
                                          resolved, LCN_PKG_MAX_DEPS);
    int path_count = 0;

    for (int i = 0; i < count && path_count < max_paths; i++) {
        /* Check for src/ subdirectory */
        char src_dir[LCN_PKG_MAX_PATH];
        snprintf(src_dir, sizeof(src_dir), "%s/src", resolved[i].path);

        struct stat st;
        if (stat(src_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(paths[path_count], src_dir, LCN_PKG_MAX_PATH - 1);
            paths[path_count][LCN_PKG_MAX_PATH - 1] = '\0';
        } else {
            /* Use package root directly */
            strncpy(paths[path_count], resolved[i].path, LCN_PKG_MAX_PATH - 1);
            paths[path_count][LCN_PKG_MAX_PATH - 1] = '\0';
        }
        path_count++;
    }

    return path_count;
}
