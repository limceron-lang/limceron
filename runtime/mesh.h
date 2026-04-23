/*
 * Limceron Runtime — Mesh Pipeline Execution
 *
 * Provides parallel fan-out / fan-in execution for mesh pipelines.
 * Uses the thread pool (threads.h) to run fan-out targets concurrently
 * and a barrier to collect fan-in results.
 *
 * Route kinds:
 *   LCN_ROUTE_SINGLE   — A -> B  (pass output of A to B)
 *   LCN_ROUTE_FAN_OUT  — A -> [B, C, D]  (run B, C, D in parallel with A's output)
 *   LCN_ROUTE_FAN_IN   — [B, C, D] -> E  (collect all results, pass to E)
 */

#ifndef LCN_MESH_H
#define LCN_MESH_H

#include <stdint.h>
#include <stdbool.h>

/* LcnString and LcnResult: guarded so they work whether mesh.h
 * is included standalone (from mesh.c) or via lcn_runtime.h. */
#ifndef LCN_STRING_TYPEDEF
#define LCN_STRING_TYPEDEF
typedef const char *LcnString;
#endif

#ifndef LCN_RESULT_TYPEDEF
#define LCN_RESULT_TYPEDEF
typedef struct {
    bool ok;
    void *value;
    LcnString error;
} LcnResult;
#endif

/* ════════════════════════════════════════════════
 * Route Kinds
 * ════════════════════════════════════════════════ */

typedef enum {
    LCN_ROUTE_SINGLE,     /* A -> B                     */
    LCN_ROUTE_FAN_OUT,    /* A -> [B, C, D]  (parallel) */
    LCN_ROUTE_FAN_IN      /* [B, C, D] -> A  (collect)  */
} LcnRouteKind;

/* ════════════════════════════════════════════════
 * Mesh Agent Function Signature
 * ════════════════════════════════════════════════ */

/* Every mesh agent function takes a string input and returns a string result.
 * On error the result is NULL and the error output param is set. */
typedef LcnString (*LcnMeshAgentFn)(LcnString input);

/* ════════════════════════════════════════════════
 * Route & Mesh Definitions
 * ════════════════════════════════════════════════ */

#define LCN_MESH_MAX_TARGETS 32

typedef struct {
    LcnRouteKind     kind;

    /* Single route: source_count=1, target_count=1
     * Fan-out:      source_count=1, target_count=N
     * Fan-in:       source_count=N, target_count=1 */
    const char      *source_names[LCN_MESH_MAX_TARGETS];
    LcnMeshAgentFn   source_fns[LCN_MESH_MAX_TARGETS];
    int              source_count;

    const char      *target_names[LCN_MESH_MAX_TARGETS];
    LcnMeshAgentFn   target_fns[LCN_MESH_MAX_TARGETS];
    int              target_count;
} LcnMeshRoute;

#define LCN_MESH_MAX_ROUTES 64

typedef struct {
    const char   *name;
    LcnMeshRoute  routes[LCN_MESH_MAX_ROUTES];
    int           route_count;
    bool          fail_fast;   /* true = abort on first error; false = collect all */
} LcnMesh;

/* ════════════════════════════════════════════════
 * Fan-out Result (per-target)
 * ════════════════════════════════════════════════ */

typedef struct {
    const char *agent_name;
    LcnString   result;       /* NULL on error */
    const char *error;        /* NULL on success */
    bool        ok;
} LcnMeshTaskResult;

/* Collected results from a fan-out or fan-in barrier */
typedef struct {
    LcnMeshTaskResult *results;
    int                count;
    int                succeeded;
    int                failed;
} LcnMeshFanResult;

/* ════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════ */

/* Create a new empty mesh */
LcnMesh *lcn_mesh_new(const char *name);

/* Add a single route: source -> target */
void lcn_mesh_add_route_single(LcnMesh *mesh,
                                const char *source_name, LcnMeshAgentFn source_fn,
                                const char *target_name, LcnMeshAgentFn target_fn);

/* Add a fan-out route: source -> [target1, target2, ...] */
void lcn_mesh_add_route_fan_out(LcnMesh *mesh,
                                 const char *source_name, LcnMeshAgentFn source_fn,
                                 const char **target_names, LcnMeshAgentFn *target_fns,
                                 int target_count);

/* Add a fan-in route: [source1, source2, ...] -> target */
void lcn_mesh_add_route_fan_in(LcnMesh *mesh,
                                const char **source_names, LcnMeshAgentFn *source_fns,
                                int source_count,
                                const char *target_name, LcnMeshAgentFn target_fn);

/* Execute the mesh pipeline with an initial input string.
 * Returns LCN_OK on success, LCN_ERR on failure.
 * The result string is stored via *output (caller must free). */
LcnResult lcn_mesh_execute(LcnMesh *mesh, LcnString input, LcnString *output);

/* Free a mesh and its internal storage */
void lcn_mesh_free(LcnMesh *mesh);

/* Execute fan-out: run N agent functions in parallel on the same input.
 * Returns collected results. Caller must free result.results. */
LcnMeshFanResult lcn_mesh_fan_out(LcnMeshAgentFn *fns, const char **names,
                                   int count, LcnString input, bool fail_fast);

/* Execute fan-in: concatenate collected results and pass to target.
 * Separator between results is "\n---\n". */
LcnString lcn_mesh_fan_in_collect(LcnMeshFanResult *fan_result);

#endif /* LCN_MESH_H */
