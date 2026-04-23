/*
 * Limceron Runtime — Mesh Pipeline Execution
 *
 * Parallel fan-out / fan-in for multi-agent mesh pipelines.
 * Uses the thread pool (threads.h) for concurrent execution.
 *
 * Compile: cc -std=c99 -O2 -Wall -c mesh.c -o mesh.o
 * Link:    ... -lpthread
 */

#define _POSIX_C_SOURCE 200112L

#include "mesh.h"
#include "threads.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════
 * Internal: Task argument for thread pool
 * ════════════════════════════════════════════════ */

typedef struct {
    LcnMeshAgentFn  fn;
    LcnString       input;
    const char     *agent_name;
} LcnMeshTaskArg;

/* Thread pool task wrapper: calls the agent function, returns result */
static void *mesh_task_wrapper(void *arg) {
    LcnMeshTaskArg *ta = (LcnMeshTaskArg *)arg;
    LcnMeshTaskResult *result = (LcnMeshTaskResult *)calloc(1, sizeof(LcnMeshTaskResult));
    if (!result) {
        fprintf(stderr, "lcn_mesh: out of memory\n");
        abort();
    }

    result->agent_name = ta->agent_name;
    result->result = NULL;
    result->error = NULL;
    result->ok = false;

    /* Call the agent function */
    LcnString output = ta->fn(ta->input);
    if (output && output[0] != '\0') {
        result->result = output;
        result->ok = true;
    } else {
        result->error = "agent returned empty or null result";
        result->ok = false;
    }

    return result;
}

/* ════════════════════════════════════════════════
 * Public API: Mesh lifecycle
 * ════════════════════════════════════════════════ */

LcnMesh *lcn_mesh_new(const char *name) {
    LcnMesh *mesh = (LcnMesh *)calloc(1, sizeof(LcnMesh));
    if (!mesh) {
        fprintf(stderr, "lcn_mesh: out of memory\n");
        abort();
    }
    mesh->name = name;
    mesh->route_count = 0;
    mesh->fail_fast = false;  /* default: collect all results */
    return mesh;
}

void lcn_mesh_add_route_single(LcnMesh *mesh,
                                const char *source_name, LcnMeshAgentFn source_fn,
                                const char *target_name, LcnMeshAgentFn target_fn) {
    if (mesh->route_count >= LCN_MESH_MAX_ROUTES) {
        fprintf(stderr, "lcn_mesh: too many routes (max %d)\n", LCN_MESH_MAX_ROUTES);
        return;
    }
    LcnMeshRoute *r = &mesh->routes[mesh->route_count++];
    r->kind = LCN_ROUTE_SINGLE;
    r->source_names[0] = source_name;
    r->source_fns[0] = source_fn;
    r->source_count = 1;
    r->target_names[0] = target_name;
    r->target_fns[0] = target_fn;
    r->target_count = 1;
}

void lcn_mesh_add_route_fan_out(LcnMesh *mesh,
                                 const char *source_name, LcnMeshAgentFn source_fn,
                                 const char **target_names, LcnMeshAgentFn *target_fns,
                                 int target_count) {
    if (mesh->route_count >= LCN_MESH_MAX_ROUTES) {
        fprintf(stderr, "lcn_mesh: too many routes (max %d)\n", LCN_MESH_MAX_ROUTES);
        return;
    }
    if (target_count > LCN_MESH_MAX_TARGETS) {
        fprintf(stderr, "lcn_mesh: too many fan-out targets (max %d)\n", LCN_MESH_MAX_TARGETS);
        return;
    }
    LcnMeshRoute *r = &mesh->routes[mesh->route_count++];
    r->kind = LCN_ROUTE_FAN_OUT;
    r->source_names[0] = source_name;
    r->source_fns[0] = source_fn;
    r->source_count = 1;
    for (int i = 0; i < target_count; i++) {
        r->target_names[i] = target_names[i];
        r->target_fns[i] = target_fns[i];
    }
    r->target_count = target_count;
}

void lcn_mesh_add_route_fan_in(LcnMesh *mesh,
                                const char **source_names, LcnMeshAgentFn *source_fns,
                                int source_count,
                                const char *target_name, LcnMeshAgentFn target_fn) {
    if (mesh->route_count >= LCN_MESH_MAX_ROUTES) {
        fprintf(stderr, "lcn_mesh: too many routes (max %d)\n", LCN_MESH_MAX_ROUTES);
        return;
    }
    if (source_count > LCN_MESH_MAX_TARGETS) {
        fprintf(stderr, "lcn_mesh: too many fan-in sources (max %d)\n", LCN_MESH_MAX_TARGETS);
        return;
    }
    LcnMeshRoute *r = &mesh->routes[mesh->route_count++];
    r->kind = LCN_ROUTE_FAN_IN;
    for (int i = 0; i < source_count; i++) {
        r->source_names[i] = source_names[i];
        r->source_fns[i] = source_fns[i];
    }
    r->source_count = source_count;
    r->target_names[0] = target_name;
    r->target_fns[0] = target_fn;
    r->target_count = 1;
}

/* ════════════════════════════════════════════════
 * Fan-out: parallel execution via thread pool
 * ════════════════════════════════════════════════ */

LcnMeshFanResult lcn_mesh_fan_out(LcnMeshAgentFn *fns, const char **names,
                                   int count, LcnString input, bool fail_fast) {
    LcnMeshFanResult result;
    result.count = count;
    result.succeeded = 0;
    result.failed = 0;
    result.results = (LcnMeshTaskResult *)calloc((size_t)count, sizeof(LcnMeshTaskResult));
    if (!result.results) {
        fprintf(stderr, "lcn_mesh: out of memory\n");
        abort();
    }

    if (count == 0) return result;

    /* Allocate task arguments */
    LcnMeshTaskArg *args = (LcnMeshTaskArg *)calloc((size_t)count, sizeof(LcnMeshTaskArg));
    if (!args) {
        fprintf(stderr, "lcn_mesh: out of memory\n");
        abort();
    }

    /* Spawn all tasks in parallel via thread pool */
    typedef struct LcnTaskHandle LcnTaskHandle;
    LcnTaskHandle **handles = (LcnTaskHandle **)calloc((size_t)count, sizeof(LcnTaskHandle *));
    if (!handles) {
        fprintf(stderr, "lcn_mesh: out of memory\n");
        abort();
    }

    fprintf(stderr, "[mesh] fan-out: dispatching %d tasks in parallel\n", count);

    for (int i = 0; i < count; i++) {
        args[i].fn = fns[i];
        args[i].input = input;
        args[i].agent_name = names[i];
        handles[i] = lcn_spawn_task(mesh_task_wrapper, &args[i]);
    }

    /* Await all tasks (barrier) */
    for (int i = 0; i < count; i++) {
        LcnMeshTaskResult *tr = (LcnMeshTaskResult *)lcn_await_task(handles[i]);
        if (tr) {
            result.results[i] = *tr;
            if (tr->ok) {
                result.succeeded++;
                fprintf(stderr, "[mesh] fan-out: %s completed (%zu bytes)\n",
                        names[i], tr->result ? strlen(tr->result) : 0);
            } else {
                result.failed++;
                fprintf(stderr, "[mesh] fan-out: %s FAILED: %s\n",
                        names[i], tr->error ? tr->error : "unknown");
                if (fail_fast) {
                    /* Mark remaining as failed */
                    for (int j = i + 1; j < count; j++) {
                        /* Still need to await remaining handles to avoid leaks */
                        LcnMeshTaskResult *rj = (LcnMeshTaskResult *)lcn_await_task(handles[j]);
                        if (rj) {
                            result.results[j] = *rj;
                            if (rj->ok) result.succeeded++;
                            else result.failed++;
                            free(rj);
                        } else {
                            result.results[j].agent_name = names[j];
                            result.results[j].ok = false;
                            result.results[j].error = "aborted (fail-fast)";
                            result.failed++;
                        }
                    }
                    free(tr);
                    break;
                }
            }
            free(tr);
        } else {
            result.results[i].agent_name = names[i];
            result.results[i].ok = false;
            result.results[i].error = "task returned null";
            result.failed++;
        }
    }

    fprintf(stderr, "[mesh] fan-out: %d/%d succeeded\n", result.succeeded, count);

    free(handles);
    free(args);
    return result;
}

/* ════════════════════════════════════════════════
 * Fan-in: collect results into a single string
 * ════════════════════════════════════════════════ */

LcnString lcn_mesh_fan_in_collect(LcnMeshFanResult *fan_result) {
    if (!fan_result || fan_result->count == 0) return "";

    /* Calculate total size */
    size_t total = 0;
    const char *sep = "\n---\n";
    size_t sep_len = strlen(sep);

    for (int i = 0; i < fan_result->count; i++) {
        if (fan_result->results[i].ok && fan_result->results[i].result) {
            if (total > 0) total += sep_len;
            total += strlen(fan_result->results[i].result);
        } else {
            /* Include error marker for failed tasks */
            if (total > 0) total += sep_len;
            total += 32; /* "[ERROR: <agent_name>]" */
            if (fan_result->results[i].agent_name)
                total += strlen(fan_result->results[i].agent_name);
            if (fan_result->results[i].error)
                total += strlen(fan_result->results[i].error) + 2;
        }
    }

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        fprintf(stderr, "lcn_mesh: out of memory\n");
        abort();
    }
    buf[0] = '\0';
    size_t pos = 0;

    for (int i = 0; i < fan_result->count; i++) {
        if (pos > 0) {
            memcpy(buf + pos, sep, sep_len);
            pos += sep_len;
        }
        if (fan_result->results[i].ok && fan_result->results[i].result) {
            size_t rlen = strlen(fan_result->results[i].result);
            memcpy(buf + pos, fan_result->results[i].result, rlen);
            pos += rlen;
        } else {
            int written = snprintf(buf + pos, total - pos + 1, "[ERROR: %s: %s]",
                                   fan_result->results[i].agent_name ?
                                       fan_result->results[i].agent_name : "?",
                                   fan_result->results[i].error ?
                                       fan_result->results[i].error : "unknown");
            if (written > 0) pos += (size_t)written;
        }
    }
    buf[pos] = '\0';

    fprintf(stderr, "[mesh] fan-in: collected %d results (%zu bytes)\n",
            fan_result->count, pos);

    return buf;
}

/* ════════════════════════════════════════════════
 * Mesh pipeline execution
 * ════════════════════════════════════════════════ */

LcnResult lcn_mesh_execute(LcnMesh *mesh, LcnString input, LcnString *output) {
    if (!mesh || !input) {
        if (output) *output = NULL;
        return (LcnResult){ .ok = false, .value = NULL, .error = "mesh or input is null" };
    }

    fprintf(stderr, "[mesh] executing '%s' with %d routes\n", mesh->name, mesh->route_count);

    LcnString current_input = input;

    for (int i = 0; i < mesh->route_count; i++) {
        LcnMeshRoute *route = &mesh->routes[i];

        switch (route->kind) {
        case LCN_ROUTE_SINGLE: {
            fprintf(stderr, "[mesh] route %d: %s -> %s (single)\n", i,
                    route->source_names[0], route->target_names[0]);

            /* Run source if it has a function (skip for "input" pseudo-node) */
            LcnString stage_out = current_input;
            if (route->source_fns[0]) {
                stage_out = route->source_fns[0](current_input);
                if (!stage_out || stage_out[0] == '\0') {
                    fprintf(stderr, "[mesh] route %d: source '%s' failed\n",
                            i, route->source_names[0]);
                    if (output) *output = NULL;
                    return (LcnResult){ .ok = false, .value = NULL,
                                        .error = "mesh single route source failed" };
                }
                fprintf(stderr, "[mesh] route %d: source '%s' -> %zu bytes\n",
                        i, route->source_names[0], strlen(stage_out));
            }

            /* Run target */
            if (route->target_fns[0]) {
                current_input = route->target_fns[0](stage_out);
                if (!current_input || current_input[0] == '\0') {
                    fprintf(stderr, "[mesh] route %d: target '%s' failed\n",
                            i, route->target_names[0]);
                    if (output) *output = NULL;
                    return (LcnResult){ .ok = false, .value = NULL,
                                        .error = "mesh single route target failed" };
                }
                fprintf(stderr, "[mesh] route %d: target '%s' -> %zu bytes\n",
                        i, route->target_names[0], strlen(current_input));
            } else {
                /* "output" pseudo-node: just pass through */
                current_input = stage_out;
            }
            break;
        }

        case LCN_ROUTE_FAN_OUT: {
            fprintf(stderr, "[mesh] route %d: %s -> [%d targets] (fan-out)\n", i,
                    route->source_names[0], route->target_count);

            /* Run source first (if not "input" pseudo-node) */
            LcnString fan_input = current_input;
            if (route->source_fns[0]) {
                fan_input = route->source_fns[0](current_input);
                if (!fan_input || fan_input[0] == '\0') {
                    fprintf(stderr, "[mesh] route %d: fan-out source '%s' failed\n",
                            i, route->source_names[0]);
                    if (output) *output = NULL;
                    return (LcnResult){ .ok = false, .value = NULL,
                                        .error = "mesh fan-out source failed" };
                }
            }

            /* Fan-out: run all targets in parallel */
            LcnMeshFanResult fan_result = lcn_mesh_fan_out(
                route->target_fns, route->target_names,
                route->target_count, fan_input, mesh->fail_fast);

            /* Collect results into a single string for next route */
            current_input = lcn_mesh_fan_in_collect(&fan_result);
            free(fan_result.results);
            break;
        }

        case LCN_ROUTE_FAN_IN: {
            fprintf(stderr, "[mesh] route %d: [%d sources] -> %s (fan-in)\n", i,
                    route->source_count, route->target_names[0]);

            /* Fan-in: run all source agents in parallel */
            LcnMeshFanResult fan_result = lcn_mesh_fan_out(
                route->source_fns, route->source_names,
                route->source_count, current_input, mesh->fail_fast);

            /* Collect and pass to target */
            LcnString collected = lcn_mesh_fan_in_collect(&fan_result);
            free(fan_result.results);

            /* Run the target with collected input */
            if (route->target_fns[0]) {
                current_input = route->target_fns[0](collected);
                if (!current_input || current_input[0] == '\0') {
                    fprintf(stderr, "[mesh] route %d: fan-in target '%s' failed\n",
                            i, route->target_names[0]);
                    if (output) *output = NULL;
                    return (LcnResult){ .ok = false, .value = NULL,
                                        .error = "mesh fan-in target failed" };
                }
            } else {
                current_input = collected;
            }
            break;
        }
        }
    }

    fprintf(stderr, "[mesh] '%s' completed successfully\n", mesh->name);

    if (output) *output = current_input;
    return (LcnResult){ .ok = true, .value = (void *)current_input, .error = NULL };
}

void lcn_mesh_free(LcnMesh *mesh) {
    if (mesh) free(mesh);
}
