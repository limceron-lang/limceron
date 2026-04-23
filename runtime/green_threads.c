/*
 * Limceron Runtime — Green Threads (M:N Scheduler) Implementation
 *
 * Cooperative M:N scheduler: M green threads multiplexed onto N OS threads.
 *
 * Architecture:
 *   - A global scheduler holds a run queue of ready green threads.
 *   - N OS worker threads pull green threads from the run queue.
 *   - Each green thread is a lightweight unit backed by a real pthread
 *     with a small stack (64KB default), but they share a pool of
 *     worker slots and a central run queue, providing M:N scheduling.
 *   - When a green thread yields, it re-enqueues itself and the worker
 *     picks the next available green thread.
 *   - When a green thread is awaited but not done, the caller blocks
 *     on a per-thread condition variable.
 *
 * Why not ucontext_t?
 *   - Deprecated on macOS (generates warnings, may be removed)
 *   - setjmp/longjmp + manual stack switching is fragile
 *   - Small-stack pthreads give us the same M:N multiplexing benefit
 *     (thousands of concurrent tasks) with full portability and debuggability.
 *
 * The key insight: the bottleneck with pthreads isn't the thread itself
 * (Linux/macOS can handle 10K+ threads), it's the default 8MB stack.
 * By using 64KB stacks, we can run thousands of green threads cheaply.
 *
 * Memory overhead per green thread:
 *   - pthread stack: 64KB (configurable via LCN_GREEN_STACK_SIZE)
 *   - GreenThread struct: ~96 bytes
 *   - pthread overhead: ~8KB (kernel thread descriptor)
 *   Total: ~72KB per green thread (vs ~8MB per default pthread)
 *
 * Compile: cc -std=c99 -O2 -Wall -c green_threads.c -o green_threads.o
 * Link:    ... -lpthread  (on Linux; macOS includes pthreads implicitly)
 */

#define _POSIX_C_SOURCE 200112L

#include "green_threads.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

/* ════════════════════════════════════════════════
 * Internal: GreenThread structure
 * ════════════════════════════════════════════════ */

struct GreenThread {
    int                  id;
    GreenThreadState     state;
    void               *(*fn)(void *);        /* task function */
    void                *arg;                  /* task argument */
    void                *result;               /* return value */
    pthread_t            thread;               /* backing OS thread */
    pthread_attr_t       attr;                 /* thread attributes (small stack) */
    size_t               stack_size;           /* configured stack size */
    pthread_mutex_t      mutex;                /* per-thread lock */
    pthread_cond_t       cond;                 /* for await + park/unpark */
    bool                 completed;            /* true when fn() returned */
    bool                 parked;               /* true when parked (waiting) */
    bool                 detached;             /* true if no one will await */
    struct GreenThread  *next;                 /* linked list for run queue */
};

/* ════════════════════════════════════════════════
 * Internal: Run queue (FIFO linked list)
 * ════════════════════════════════════════════════ */

typedef struct {
    GreenThread    *head;
    GreenThread    *tail;
    int             count;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;     /* signalled when a task is enqueued */
} GreenRunQueue;

/* ════════════════════════════════════════════════
 * Internal: Scheduler state
 * ════════════════════════════════════════════════ */

typedef struct {
    GreenRunQueue   queue;
    int             next_id;          /* monotonic ID counter */
    int             alive_count;      /* green threads not yet DONE */
    int             worker_count;     /* number of OS worker threads */
    bool            initialized;
    bool            shutdown;
    pthread_mutex_t lock;             /* protects next_id, alive_count, shutdown */
    pthread_cond_t  all_done;         /* signalled when alive_count hits 0 */
} GreenScheduler;

/* Global singleton */
static GreenScheduler g_sched;
static pthread_mutex_t g_green_init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ════════════════════════════════════════════════
 * Run queue operations
 * ════════════════════════════════════════════════ */

static void rq_init(GreenRunQueue *q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void rq_push(GreenRunQueue *q, GreenThread *gt) {
    gt->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (q->tail) {
        q->tail->next = gt;
    } else {
        q->head = gt;
    }
    q->tail = gt;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

/* Pop from front. Returns NULL if empty. Does not block. */
static GreenThread *rq_try_pop(GreenRunQueue *q) {
    pthread_mutex_lock(&q->mutex);
    GreenThread *gt = q->head;
    if (gt) {
        q->head = gt->next;
        if (!q->head) q->tail = NULL;
        q->count--;
        gt->next = NULL;
    }
    pthread_mutex_unlock(&q->mutex);
    return gt;
}

static void rq_destroy(GreenRunQueue *q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
}

/* ════════════════════════════════════════════════
 * Worker thread: pulls green threads from run queue
 * and launches them. This provides the M:N scheduling
 * — the worker pool limits concurrent OS threads while
 * allowing many more green threads to exist.
 *
 * Design: Workers don't use a persistent thread that
 * context-switches between green threads. Instead, each
 * green thread IS a small-stack pthread. The "M:N" comes
 * from the run queue acting as a throttle: we only let
 * N green threads run concurrently by limiting how many
 * we launch at once.
 *
 * For the initial implementation, we launch each green
 * thread immediately on spawn (no worker pool bottleneck).
 * The small stack size is what makes this scale.
 * ════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════
 * Resolve worker count
 * ════════════════════════════════════════════════ */

static int resolve_green_workers(int requested) {
    if (requested > 0) {
        return requested < LCN_MAX_WORKERS ? requested : LCN_MAX_WORKERS;
    }

    const char *env = getenv("LCN_GREEN_WORKERS");
    if (env && env[0] != '\0') {
        int n = atoi(env);
        if (n > 0) return n < LCN_MAX_WORKERS ? n : LCN_MAX_WORKERS;
    }

    /* Auto-detect CPU count */
#if defined(_SC_NPROCESSORS_ONLN)
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n > 0) return (int)(n < LCN_MAX_WORKERS ? n : LCN_MAX_WORKERS);
    }
#endif

    return 4;  /* fallback */
}

/* ════════════════════════════════════════════════
 * Semaphore-like throttle for concurrent green threads
 *
 * Limits how many green threads actually run at once
 * (the N in M:N). Uses a mutex + condvar counter.
 * ════════════════════════════════════════════════ */

typedef struct {
    int             max_concurrent;
    int             active;
    pthread_mutex_t mutex;
    pthread_cond_t  available;
} GreenThrottle;

static GreenThrottle g_throttle;

static void throttle_init(GreenThrottle *t, int max_conc) {
    t->max_concurrent = max_conc;
    t->active = 0;
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->available, NULL);
}

static void throttle_acquire(GreenThrottle *t) {
    pthread_mutex_lock(&t->mutex);
    while (t->active >= t->max_concurrent) {
        pthread_cond_wait(&t->available, &t->mutex);
    }
    t->active++;
    pthread_mutex_unlock(&t->mutex);
}

static void throttle_release(GreenThrottle *t) {
    pthread_mutex_lock(&t->mutex);
    t->active--;
    pthread_cond_signal(&t->available);
    pthread_mutex_unlock(&t->mutex);
}

static void throttle_destroy(GreenThrottle *t) {
    pthread_mutex_destroy(&t->mutex);
    pthread_cond_destroy(&t->available);
}

/* ════════════════════════════════════════════════
 * Throttled green thread entry
 *
 * Wraps green_thread_entry with throttle accounting
 * so that at most N green threads execute concurrently.
 * ════════════════════════════════════════════════ */

static void *throttled_green_entry(void *arg) {
    GreenThread *gt = (GreenThread *)arg;

    /* Acquire a slot (blocks if N already running) */
    throttle_acquire(&g_throttle);

    /* Run the actual green thread logic */
    void *result = NULL;

    /* Mark as running */
    pthread_mutex_lock(&gt->mutex);
    gt->state = GT_RUNNING;
    pthread_mutex_unlock(&gt->mutex);

    /* Execute the task function */
    result = gt->fn(gt->arg);

    /* Mark complete */
    pthread_mutex_lock(&gt->mutex);
    gt->result = result;
    gt->state = GT_DONE;
    gt->completed = true;
    pthread_cond_broadcast(&gt->cond);
    pthread_mutex_unlock(&gt->mutex);

    /* Decrement alive count */
    pthread_mutex_lock(&g_sched.lock);
    g_sched.alive_count--;
    if (g_sched.alive_count == 0) {
        pthread_cond_signal(&g_sched.all_done);
    }
    pthread_mutex_unlock(&g_sched.lock);

    /* Release the throttle slot */
    throttle_release(&g_throttle);

    return result;
}

/* ════════════════════════════════════════════════
 * Dispatcher thread: monitors the run queue and
 * launches green threads as throttle slots open up.
 * ════════════════════════════════════════════════ */

static pthread_t g_dispatcher;
static bool g_dispatcher_running = false;

static void *dispatcher_loop(void *arg) {
    (void)arg;

    for (;;) {
        /* Check shutdown */
        pthread_mutex_lock(&g_sched.lock);
        bool done = g_sched.shutdown;
        pthread_mutex_unlock(&g_sched.lock);
        if (done) break;

        /* Try to pop a green thread from the run queue */
        GreenThread *gt = rq_try_pop(&g_sched.queue);
        if (!gt) {
            /* No work available — wait on the run queue condvar */
            pthread_mutex_lock(&g_sched.queue.mutex);

            /* Re-check after acquiring lock */
            pthread_mutex_lock(&g_sched.lock);
            done = g_sched.shutdown;
            pthread_mutex_unlock(&g_sched.lock);
            if (done) {
                pthread_mutex_unlock(&g_sched.queue.mutex);
                break;
            }

            if (!g_sched.queue.head) {
                /* Use timed wait to periodically check shutdown */
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 50 * 1000000L;  /* 50ms */
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&g_sched.queue.not_empty,
                                       &g_sched.queue.mutex, &ts);
            }
            pthread_mutex_unlock(&g_sched.queue.mutex);
            continue;
        }

        /* We have a green thread to launch.
         * Set up its pthread with a small stack and launch it. */
        pthread_attr_init(&gt->attr);

        /* Set small stack size. The minimum varies by platform;
         * ensure we respect PTHREAD_STACK_MIN. */
        size_t stack_sz = gt->stack_size;
#ifdef PTHREAD_STACK_MIN
        if (stack_sz < (size_t)PTHREAD_STACK_MIN) {
            stack_sz = (size_t)PTHREAD_STACK_MIN;
        }
#endif
        pthread_attr_setstacksize(&gt->attr, stack_sz);

        /* Set detached — the green thread will clean up via await or auto-cleanup */
        pthread_attr_setdetachstate(&gt->attr, PTHREAD_CREATE_JOINABLE);

        int rc = pthread_create(&gt->thread, &gt->attr, throttled_green_entry, gt);
        if (rc != 0) {
            fprintf(stderr, "lcn_green: pthread_create failed (errno=%d): %s\n",
                    rc, strerror(rc));

            /* Mark as done with error */
            pthread_mutex_lock(&gt->mutex);
            gt->state = GT_DONE;
            gt->completed = true;
            gt->result = NULL;
            pthread_cond_broadcast(&gt->cond);
            pthread_mutex_unlock(&gt->mutex);

            pthread_mutex_lock(&g_sched.lock);
            g_sched.alive_count--;
            if (g_sched.alive_count == 0) {
                pthread_cond_signal(&g_sched.all_done);
            }
            pthread_mutex_unlock(&g_sched.lock);

            pthread_attr_destroy(&gt->attr);
        }
    }

    return NULL;
}

/* ════════════════════════════════════════════════
 * Public API: Initialization
 * ════════════════════════════════════════════════ */

void lcn_green_init_with_workers(int num_workers) {
    pthread_mutex_lock(&g_green_init_mutex);

    if (g_sched.initialized) {
        pthread_mutex_unlock(&g_green_init_mutex);
        return;
    }

    int n = resolve_green_workers(num_workers);

    /* Initialize scheduler */
    memset(&g_sched, 0, sizeof(g_sched));
    g_sched.worker_count = n;
    g_sched.next_id = 1;
    g_sched.alive_count = 0;
    g_sched.shutdown = false;
    pthread_mutex_init(&g_sched.lock, NULL);
    pthread_cond_init(&g_sched.all_done, NULL);

    /* Initialize run queue */
    rq_init(&g_sched.queue);

    /* Initialize throttle (limits concurrent green threads to N) */
    throttle_init(&g_throttle, n);

    /* Start dispatcher thread */
    g_dispatcher_running = true;
    int rc = pthread_create(&g_dispatcher, NULL, dispatcher_loop, NULL);
    if (rc != 0) {
        fprintf(stderr, "lcn_green: failed to create dispatcher thread\n");
        g_dispatcher_running = false;
    }

    g_sched.initialized = true;
    pthread_mutex_unlock(&g_green_init_mutex);
}

void lcn_green_init(void) {
    lcn_green_init_with_workers(0);
}

/* ════════════════════════════════════════════════
 * Public API: Spawn
 * ════════════════════════════════════════════════ */

GreenThread *lcn_green_spawn(void *(*fn)(void *), void *arg) {
    /* Auto-init if not yet initialized */
    if (!g_sched.initialized) {
        lcn_green_init();
    }

    /* Allocate green thread */
    GreenThread *gt = (GreenThread *)calloc(1, sizeof(GreenThread));
    if (!gt) {
        fprintf(stderr, "lcn_green: out of memory\n");
        abort();
    }

    /* Assign ID */
    pthread_mutex_lock(&g_sched.lock);
    gt->id = g_sched.next_id++;
    g_sched.alive_count++;
    pthread_mutex_unlock(&g_sched.lock);

    gt->fn = fn;
    gt->arg = arg;
    gt->result = NULL;
    gt->state = GT_READY;
    gt->stack_size = LCN_GREEN_STACK_SIZE;
    gt->completed = false;
    gt->parked = false;
    gt->detached = false;
    gt->next = NULL;
    pthread_mutex_init(&gt->mutex, NULL);
    pthread_cond_init(&gt->cond, NULL);

    /* Add to run queue — dispatcher will launch it */
    rq_push(&g_sched.queue, gt);

    return gt;
}

/* ════════════════════════════════════════════════
 * Public API: Yield
 * ════════════════════════════════════════════════ */

void lcn_green_yield(void) {
    /* In the small-stack pthread model, yield just calls sched_yield()
     * which tells the OS to give other threads a chance to run.
     * This is the portable equivalent of a cooperative yield point. */
#if defined(_POSIX_C_SOURCE) || defined(__APPLE__)
    sched_yield();
#endif
}

/* ════════════════════════════════════════════════
 * Public API: Await
 * ════════════════════════════════════════════════ */

void *lcn_green_await(GreenThread *gt) {
    if (!gt) return NULL;

    /* Wait for completion */
    pthread_mutex_lock(&gt->mutex);
    while (!gt->completed) {
        pthread_cond_wait(&gt->cond, &gt->mutex);
    }
    void *result = gt->result;
    pthread_mutex_unlock(&gt->mutex);

    /* Join the backing pthread to avoid resource leaks */
    pthread_join(gt->thread, NULL);

    /* Cleanup */
    pthread_attr_destroy(&gt->attr);
    pthread_mutex_destroy(&gt->mutex);
    pthread_cond_destroy(&gt->cond);
    free(gt);

    return result;
}

/* ════════════════════════════════════════════════
 * Public API: Shutdown
 * ════════════════════════════════════════════════ */

void lcn_green_shutdown(void) {
    pthread_mutex_lock(&g_green_init_mutex);

    if (!g_sched.initialized) {
        pthread_mutex_unlock(&g_green_init_mutex);
        return;
    }

    /* Wait for all alive green threads to finish */
    pthread_mutex_lock(&g_sched.lock);
    while (g_sched.alive_count > 0) {
        pthread_cond_wait(&g_sched.all_done, &g_sched.lock);
    }

    /* Signal shutdown to dispatcher */
    g_sched.shutdown = true;
    pthread_mutex_unlock(&g_sched.lock);

    /* Wake dispatcher in case it's waiting on the queue */
    pthread_mutex_lock(&g_sched.queue.mutex);
    pthread_cond_signal(&g_sched.queue.not_empty);
    pthread_mutex_unlock(&g_sched.queue.mutex);

    /* Join dispatcher thread */
    if (g_dispatcher_running) {
        pthread_join(g_dispatcher, NULL);
        g_dispatcher_running = false;
    }

    /* Drain any remaining items in the run queue (shouldn't happen) */
    GreenThread *orphan;
    while ((orphan = rq_try_pop(&g_sched.queue)) != NULL) {
        pthread_mutex_destroy(&orphan->mutex);
        pthread_cond_destroy(&orphan->cond);
        free(orphan);
    }

    /* Cleanup */
    rq_destroy(&g_sched.queue);
    throttle_destroy(&g_throttle);
    pthread_mutex_destroy(&g_sched.lock);
    pthread_cond_destroy(&g_sched.all_done);

    g_sched.initialized = false;
    g_sched.shutdown = false;

    pthread_mutex_unlock(&g_green_init_mutex);
}

/* ════════════════════════════════════════════════
 * Public API: Status queries
 * ════════════════════════════════════════════════ */

bool lcn_green_active(void) {
    return g_sched.initialized;
}

int lcn_green_thread_count(void) {
    pthread_mutex_lock(&g_sched.lock);
    int count = g_sched.alive_count;
    pthread_mutex_unlock(&g_sched.lock);
    return count;
}

GreenThreadState lcn_green_thread_state(GreenThread *gt) {
    if (!gt) return GT_DONE;
    pthread_mutex_lock(&gt->mutex);
    GreenThreadState s = gt->state;
    pthread_mutex_unlock(&gt->mutex);
    return s;
}

int lcn_green_thread_id(GreenThread *gt) {
    if (!gt) return -1;
    return gt->id;
}

/* ════════════════════════════════════════════════
 * Park / Unpark (channel integration)
 * ════════════════════════════════════════════════ */

void lcn_green_park(GreenThread *gt) {
    if (!gt) return;

    pthread_mutex_lock(&gt->mutex);
    gt->state = GT_WAITING;
    gt->parked = true;
    while (gt->parked) {
        pthread_cond_wait(&gt->cond, &gt->mutex);
    }
    gt->state = GT_RUNNING;
    pthread_mutex_unlock(&gt->mutex);
}

void lcn_green_unpark(GreenThread *gt) {
    if (!gt) return;

    pthread_mutex_lock(&gt->mutex);
    gt->parked = false;
    gt->state = GT_READY;
    pthread_cond_signal(&gt->cond);
    pthread_mutex_unlock(&gt->mutex);
}
