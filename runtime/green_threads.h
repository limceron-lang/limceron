/*
 * Limceron Runtime — Green Threads (M:N Scheduler)
 *
 * Cooperative M:N threading: M green threads multiplexed onto N OS threads.
 * Each green thread gets a small stack (default 64KB) and yields cooperatively.
 *
 * Uses POSIX threads with small stacks as the portable coroutine mechanism.
 * This avoids the deprecated ucontext API on macOS and works on both
 * macOS and Linux without platform-specific code.
 *
 * Usage from generated code:
 *   lcn_green_init();
 *   GreenThread *gt = lcn_green_spawn(my_fn, my_arg);
 *   void *result    = lcn_green_await(gt);
 *   lcn_green_shutdown();
 */

#ifndef LCN_GREEN_THREADS_H
#define LCN_GREEN_THREADS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ════════════════════════════════════════════════
 * Configuration
 * ════════════════════════════════════════════════ */

/* Default green thread stack size: 64 KB */
#ifndef LCN_GREEN_STACK_SIZE
#define LCN_GREEN_STACK_SIZE (64 * 1024)
#endif

/* Max OS worker threads */
#ifndef LCN_MAX_WORKERS
#define LCN_MAX_WORKERS 64
#endif

/* Default worker count (0 = auto-detect from CPU count) */
#ifndef LCN_GREEN_DEFAULT_WORKERS
#define LCN_GREEN_DEFAULT_WORKERS 0
#endif

/* ════════════════════════════════════════════════
 * Types
 * ════════════════════════════════════════════════ */

typedef enum {
    GT_READY,       /* can run — sitting in run queue */
    GT_RUNNING,     /* currently executing on a worker */
    GT_YIELDED,     /* voluntarily yielded, re-queued */
    GT_WAITING,     /* parked — waiting on I/O, channel, or await */
    GT_DONE         /* completed, result available */
} GreenThreadState;

/* Opaque green thread handle */
typedef struct GreenThread GreenThread;

/* ════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════ */

/* Initialize the green thread scheduler with N OS worker threads.
 * Pass 0 to auto-detect from LCN_GREEN_WORKERS env or CPU count.
 * Safe to call multiple times (no-op after first init). */
void lcn_green_init(void);

/* Initialize with explicit worker count. */
void lcn_green_init_with_workers(int num_workers);

/* Spawn a new green thread. The function fn(arg) will be executed
 * on one of the worker threads. Returns a handle for await.
 * The handle is valid until lcn_green_await() consumes it. */
GreenThread *lcn_green_spawn(void *(*fn)(void *), void *arg);

/* Cooperative yield: the current green thread voluntarily gives up
 * its time slice and goes back to the run queue. Other green threads
 * get a chance to run. This is a no-op if called outside a green thread. */
void lcn_green_yield(void);

/* Wait for a green thread to complete and return its result.
 * If the green thread is not done yet, the calling OS thread will
 * block until it completes. Frees the green thread handle.
 * Returns the value returned by fn(arg). */
void *lcn_green_await(GreenThread *gt);

/* Shutdown the scheduler. Waits for all pending green threads to finish,
 * then joins all OS worker threads. Safe to call multiple times. */
void lcn_green_shutdown(void);

/* Check if the green thread scheduler is initialized. */
bool lcn_green_active(void);

/* ════════════════════════════════════════════════
 * Park / Unpark (for channel integration)
 * ════════════════════════════════════════════════ */

/* Park the current green thread until another thread calls unpark on it.
 * Used by channel recv when no data is available.
 * NOTE: In the portable pthread-based implementation, park blocks the
 * current OS thread (the green thread's executor) until woken. */
void lcn_green_park(GreenThread *gt);

/* Wake a parked green thread, moving it back to READY state.
 * Used by channel send to wake a waiting receiver. */
void lcn_green_unpark(GreenThread *gt);

/* ════════════════════════════════════════════════
 * Introspection
 * ════════════════════════════════════════════════ */

/* Return the number of green threads currently alive (not DONE). */
int lcn_green_thread_count(void);

/* Return the state of a green thread. */
GreenThreadState lcn_green_thread_state(GreenThread *gt);

/* Return the ID of a green thread. */
int lcn_green_thread_id(GreenThread *gt);

#endif /* LCN_GREEN_THREADS_H */
