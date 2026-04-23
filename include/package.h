/*
 * Limceron Package Manager — Header
 *
 * TOML manifest parsing, semver, dependency resolution, lock files.
 */

#ifndef LCN_PACKAGE_H
#define LCN_PACKAGE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ============================================================
 * Configuration
 * ============================================================ */

#define LCN_PKG_MAX_DEPS       64
#define LCN_PKG_MAX_NAME       128
#define LCN_PKG_MAX_VERSION    32
#define LCN_PKG_MAX_DESC       512
#define LCN_PKG_MAX_PATH       512
#define LCN_PKG_MAX_LOCK_ENTRIES 128
#define LCN_PKG_HASH_LEN      65   /* sha256 hex + NUL */

/* ============================================================
 * Semver
 * ============================================================ */

typedef struct {
    int major;
    int minor;
    int patch;
} LcnSemver;

/* Parse "1.2.3" into components. Returns true on success. */
bool semver_parse(const char *str, LcnSemver *out);

/* Compare two semver strings: <0 if a<b, 0 if equal, >0 if a>b */
int semver_compare(const char *a, const char *b);

/* Check if `version` satisfies `constraint`.
 * Supports:
 *   "1.2.0"   exact match
 *   "^1.2.0"  compatible (>=1.2.0 <2.0.0)
 *   "*"       any version
 */
bool semver_satisfies(const char *version, const char *constraint);

/* ============================================================
 * Dependency
 * ============================================================ */

typedef enum {
    LCN_DEP_REGISTRY,   /* version string from registry (future) */
    LCN_DEP_PATH,       /* local path dependency */
    LCN_DEP_GIT,        /* git repository */
} LcnDepKind;

typedef struct {
    char name[LCN_PKG_MAX_NAME];
    char version[LCN_PKG_MAX_VERSION];   /* version constraint or "" */
    LcnDepKind kind;
    char path[LCN_PKG_MAX_PATH];         /* for path deps */
    char git_url[LCN_PKG_MAX_PATH];      /* for git deps */
    char git_rev[LCN_PKG_MAX_VERSION];   /* optional git rev/tag */
    bool is_dev;                          /* dev-dependency? */
} LcnDependency;

/* ============================================================
 * Package Manifest (limceron.toml)
 * ============================================================ */

typedef struct {
    char name[LCN_PKG_MAX_NAME];
    char version[LCN_PKG_MAX_VERSION];
    char description[LCN_PKG_MAX_DESC];

    LcnDependency deps[LCN_PKG_MAX_DEPS];
    int dep_count;
} LcnPackage;

/* Parse a limceron.toml string into LcnPackage.
 * Returns true on success, false on parse error. */
bool lcn_package_parse(const char *toml, size_t len, LcnPackage *out);

/* Serialize LcnPackage back to TOML string.
 * Writes into buf (up to bufsize). Returns chars written. */
int lcn_package_serialize(const LcnPackage *pkg, char *buf, size_t bufsize);

/* Read and parse limceron.toml from a directory.
 * `dir` is the directory containing limceron.toml.
 * Returns true on success. */
bool lcn_package_load(const char *dir, LcnPackage *out);

/* Write limceron.toml to a directory. Returns true on success. */
bool lcn_package_save(const char *dir, const LcnPackage *pkg);

/* Add a dependency to a package. Returns true on success. */
bool lcn_package_add_dep(LcnPackage *pkg, const char *name,
                         const char *version, LcnDepKind kind,
                         const char *path_or_url, bool is_dev);

/* Remove a dependency by name. Returns true if found and removed. */
bool lcn_package_remove_dep(LcnPackage *pkg, const char *name);

/* ============================================================
 * Resolved Dependency
 * ============================================================ */

typedef struct {
    char name[LCN_PKG_MAX_NAME];
    char resolved_version[LCN_PKG_MAX_VERSION];
    char path[LCN_PKG_MAX_PATH];   /* local path to package source */
    char hash[LCN_PKG_HASH_LEN];   /* sha256 of package content */
} LcnResolvedDep;

/* Resolve all dependencies for a package.
 * Writes resolved deps into `out`, up to `max` entries.
 * Returns number of resolved deps, or -1 on error. */
int lcn_resolve_dependencies(const LcnPackage *pkg, const char *project_dir,
                             LcnResolvedDep *out, int max);

/* ============================================================
 * Lock File (limceron.lock)
 * ============================================================ */

typedef struct {
    LcnResolvedDep entries[LCN_PKG_MAX_LOCK_ENTRIES];
    int count;
} LcnLockFile;

/* Parse a lock file string. Returns true on success. */
bool lcn_lock_parse(const char *content, size_t len, LcnLockFile *out);

/* Serialize lock file to string. Returns chars written. */
int lcn_lock_serialize(const LcnLockFile *lock, char *buf, size_t bufsize);

/* Read lock file from directory. Returns true on success. */
bool lcn_lock_load(const char *dir, LcnLockFile *out);

/* Write lock file to directory. Returns true on success. */
bool lcn_lock_save(const char *dir, const LcnLockFile *lock);

/* ============================================================
 * Package Installation
 * ============================================================ */

/* Install a single dependency to .limceron/packages/.
 * Returns true on success. */
bool lcn_install_dep(const char *project_dir, const LcnDependency *dep);

/* Install all dependencies from a package manifest.
 * Returns number of packages installed, or -1 on error. */
int lcn_install_all(const char *project_dir, const LcnPackage *pkg);

/* ============================================================
 * Package Publishing
 * ============================================================ */

/* Create a tarball of the package for distribution.
 * `dir` is the project directory, `out_path` receives the tarball path.
 * Returns true on success. */
bool lcn_publish_package(const char *dir, char *out_path, size_t out_path_size);

/* ============================================================
 * Build Integration
 * ============================================================ */

/* Collect import paths from all resolved dependencies.
 * Populates `paths` array with src/ directories.
 * Returns number of paths written. */
int lcn_dep_import_paths(const char *project_dir, const LcnPackage *pkg,
                         char paths[][LCN_PKG_MAX_PATH], int max_paths);

#endif /* LCN_PACKAGE_H */
