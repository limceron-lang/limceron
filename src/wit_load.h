/*
 * Limceron Compiler -- WIT Contract Loader
 *
 * Loads `include/vdag.wit` (the canonical Visual-DAG host-import
 * contract) into a small in-memory table the typechecker queries
 * when resolving an AST_HOST_CALL.
 *
 * Pairs with src/wit_emit.c (which writes the agent-side WIT
 * artefact) and src/typecheck.c (which calls
 * lcn_wit_lookup_signature during the host-call pass).
 *
 * The loader is intentionally minimal -- it only parses
 * `interface <ns> { name: func(arg: type, ...) -> type; }`
 * lines, the subset wit_emit.c already produces. Comments,
 * package headers, and `world` blocks are skipped.
 */

#ifndef LCN_WIT_LOAD_H
#define LCN_WIT_LOAD_H

#include "lcn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Capacities sized to comfortably fit the canonical vdag.wit;
 * exceeding them is a configuration error and would be flagged at
 * load time. */
#define LCN_WIT_MAX_FUNCS    128
#define LCN_WIT_MAX_PARAMS   8
#define LCN_WIT_MAX_NAME     64
#define LCN_WIT_MAX_TYPE     32
/* L5 host-error enum. Sized for the canonical vdag.errors.wit
 * (12 entries today, headroom for future sentinels). */
#define LCN_WIT_MAX_ENUMS    8
#define LCN_WIT_MAX_VARIANTS 64

typedef struct {
    char name[LCN_WIT_MAX_NAME];      /* parameter name (kebab as in WIT) */
    char type[LCN_WIT_MAX_TYPE];      /* WIT type token (string, s64, ...) */
} LcnWitParam;

typedef struct {
    char         qualified[LCN_WIT_MAX_NAME];  /* "<ns>.<verb>" matched on */
    char         ret_type[LCN_WIT_MAX_TYPE];   /* WIT return type or "_" */
    LcnWitParam  params[LCN_WIT_MAX_PARAMS];
    int          param_count;
} LcnWitFunc;

/* L5: a single (name, integer) pair inside an enum block. The
 * canonical host-error enum lives in include/vdag.errors.wit; values
 * are signed because every HostErr* sentinel is negative. */
typedef struct {
    char    name[LCN_WIT_MAX_NAME];   /* kebab-case as written in WIT */
    int64_t value;                    /* signed sentinel (e.g. -3) */
} LcnWitEnumVariant;

typedef struct {
    char              name[LCN_WIT_MAX_NAME];                 /* "host-error" */
    LcnWitEnumVariant variants[LCN_WIT_MAX_VARIANTS];
    int               variant_count;
} LcnWitEnum;

typedef struct {
    LcnWitFunc funcs[LCN_WIT_MAX_FUNCS];
    int        count;
    LcnWitEnum enums[LCN_WIT_MAX_ENUMS];
    int        enum_count;
    int        loaded;        /* 1 once a load attempt has run */
    int        load_ok;       /* 1 if the canonical file parsed OK */
    char       source_path[512];
} LcnWitContract;

/* Locate the canonical include/vdag.wit relative to the running
 * compiler binary (argv0). Writes the absolute path into `out` and
 * returns it; returns NULL if the include directory cannot be
 * located. Path resolution mirrors find_runtime_dir / find_stdlib_dir
 * in main.c. */
const char *lcn_wit_default_path(char *out, size_t out_cap,
                                 const char *argv0);

/* Load the contract file at `path` into `contract`. Returns 0 on
 * success. Soft failures (missing file, malformed lines) are logged
 * to stderr but never abort the compile -- the typecheck pass
 * silently degrades to "warn and continue" when the contract is
 * unavailable. */
int lcn_wit_load(LcnWitContract *contract, const char *path);

/* Look up the signature of a qualified host call. Returns NULL when
 * the contract does not declare the call (caller should emit a
 * warning and fall through to the legacy emit-WIT behaviour). */
const LcnWitFunc *lcn_wit_lookup_signature(const LcnWitContract *contract,
                                           const char *qualified);

/* L5: load the canonical HostError enum file (default
 * `include/vdag.errors.wit`) into `contract`. The enums and the
 * func table coexist; calling this AFTER `lcn_wit_load` extends an
 * already-loaded contract without dropping the func entries. */
const char *lcn_wit_default_errors_path(char *out, size_t out_cap,
                                        const char *argv0);
int lcn_wit_load_errors(LcnWitContract *contract, const char *path);

/* L5: find an enum declared in the loaded contract by name. Returns
 * NULL if the contract has no enum block, or the name does not
 * match. The name is matched with `-` and `_` treated as equivalent
 * (kebab/snake bridge). */
const LcnWitEnum *lcn_wit_lookup_enum(const LcnWitContract *contract,
                                      const char *name);

/* L5: case-insensitive lookup of a variant inside an enum, accepting
 * either kebab-case (`quota-exceeded`) or snake_case
 * (`quota_exceeded`) as the user-facing spelling. Returns NULL if
 * the variant is absent. */
const LcnWitEnumVariant *
lcn_wit_lookup_enum_variant(const LcnWitEnum *e, const char *name);

/* L5: process-wide shared HostError table. The first call lazily
 * loads include/vdag.errors.wit (best-effort -- a missing file means
 * `out_value` will fail to resolve and the caller falls back to 0).
 * Returns 1 if (enum_name, variant_name) resolved to a value, 0
 * otherwise. This bridges the gap between typecheck (which owns the
 * static contract) and ir_gen (which only needs the integer). */
int lcn_wit_resolve_host_error(const char *enum_name,
                               const char *variant_name,
                               int64_t *out_value);

#ifdef __cplusplus
}
#endif

#endif /* LCN_WIT_LOAD_H */
