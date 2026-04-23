/*
 * Limceron Runtime — Thread Pool
 *
 * A simple pthreads-based thread pool for the `spawn` / `await` language
 * constructs.  Worker count is configurable via lcn_threadpool_init() or
 * the LCN_WORKERS environment variable (default: 4).
 *
 * Usage from generated code:
 *   LcnTaskHandle *h = lcn_spawn_task(my_fn, my_arg);
 *   void *result     = lcn_await_task(h);
 */

#ifndef LCN_THREADS_H
#define LCN_THREADS_H

#include <stdint.h>
#include <stdbool.h>

/* Opaque handle for a spawned task */
typedef struct LcnTaskHandle LcnTaskHandle;

/* Task function signature: takes void* arg, returns void* result */
typedef void *(*LcnTaskFn)(void *arg);

/* Initialize the thread pool.  Call once at startup.
 * num_workers = 0 means use LCN_WORKERS env var or default (4). */
void lcn_threadpool_init(int num_workers);

/* Submit a task to the pool.  Returns a handle for await. */
LcnTaskHandle *lcn_spawn_task(LcnTaskFn fn, void *arg);

/* Wait for a task to complete and return its result.  Frees the handle. */
void *lcn_await_task(LcnTaskHandle *handle);

/* Shutdown the thread pool.  Waits for all pending tasks. */
void lcn_threadpool_shutdown(void);

/* Check if the thread pool is initialized. */
bool lcn_threadpool_active(void);

/* ════════════════════════════════════════════════
 * Structured Concurrency — TaskGroup
 *
 * A TaskGroup ensures all spawned tasks complete before the
 * group scope exits.  Tasks cannot escape the group.
 *
 * Usage:
 *   LcnTaskGroup *tg = lcn_task_group_new();
 *   lcn_task_group_spawn(tg, fn1, arg1);
 *   lcn_task_group_spawn(tg, fn2, arg2);
 *   void **results = lcn_task_group_await_all(tg);
 *   // results[0] = fn1 result, results[1] = fn2 result
 *   lcn_task_group_free(tg);
 * ════════════════════════════════════════════════ */

typedef struct LcnTaskGroup LcnTaskGroup;

/* Create a new empty task group. */
LcnTaskGroup *lcn_task_group_new(void);

/* Spawn a task into the group.  The task is submitted to the thread pool. */
void lcn_task_group_spawn(LcnTaskGroup *tg, LcnTaskFn fn, void *arg);

/* Block until ALL tasks in the group complete.
 * Returns a malloc'd array of void* results (one per spawn, in order).
 * Caller must free the returned array.  Returns NULL if group is empty. */
void **lcn_task_group_await_all(LcnTaskGroup *tg);

/* Free the task group (does NOT free the results array). */
void lcn_task_group_free(LcnTaskGroup *tg);

#endif /* LCN_THREADS_H */
