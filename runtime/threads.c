/*
 * Limceron Runtime — Thread Pool Implementation
 *
 * POSIX pthreads-based worker pool.  Tasks are queued into a shared
 * linked list protected by a mutex.  Each worker thread loops: dequeue
 * a task, execute it, mark it complete, and signal its per-task condvar
 * so that lcn_await_task() can return.
 *
 * Compile: cc -std=c99 -O2 -Wall -c threads.c -o threads.o
 * Link:    ... -lpthread  (on Linux; macOS includes pthreads implicitly)
 */

#define _POSIX_C_SOURCE 200112L

#include "threads.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

/* ════════════════════════════════════════════════
 * Internal: Task handle (exposed as opaque LcnTaskHandle)
 * ════════════════════════════════════════════════ */

struct LcnTaskHandle {
    LcnTaskFn       fn;
    void           *arg;
    void           *result;
    bool            completed;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
};

/* ════════════════════════════════════════════════
 * Internal: Task queue (singly-linked list)
 * ════════════════════════════════════════════════ */

typedef struct LcnQueueNode {
    LcnTaskHandle       *task;
    struct LcnQueueNode *next;
} LcnQueueNode;

typedef struct {
    LcnQueueNode   *head;
    LcnQueueNode   *tail;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;       /* signalled when a task is enqueued */
    bool            shutdown;   /* set to true during shutdown */
} LcnTaskQueue;

/* ════════════════════════════════════════════════
 * Internal: Thread pool state
 * ════════════════════════════════════════════════ */

typedef struct {
    pthread_t   *workers;
    int          num_workers;
    LcnTaskQueue queue;
    bool         initialized;
} LcnThreadPool;

/* Global singleton — not exported */
static LcnThreadPool g_pool;

/* Mutex protecting g_pool.initialized for auto-init */
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Default worker count */
#define LCN_DEFAULT_WORKERS 4

/* ════════════════════════════════════════════════
 * Queue helpers (caller must hold queue.mutex)
 * ════════════════════════════════════════════════ */

static void queue_push(LcnTaskQueue *q, LcnTaskHandle *task) {
    LcnQueueNode *node = (LcnQueueNode *)malloc(sizeof(LcnQueueNode));
    if (!node) {
        fprintf(stderr, "lcn_threads: out of memory\n");
        abort();
    }
    node->task = task;
    node->next = NULL;

    if (q->tail) {
        q->tail->next = node;
    } else {
        q->head = node;
    }
    q->tail = node;
}

static LcnTaskHandle *queue_pop(LcnTaskQueue *q) {
    if (!q->head) return NULL;

    LcnQueueNode *node = q->head;
    LcnTaskHandle *task = node->task;
    q->head = node->next;
    if (!q->head) {
        q->tail = NULL;
    }
    free(node);
    return task;
}

/* ════════════════════════════════════════════════
 * Worker thread entry point
 * ════════════════════════════════════════════════ */

static void *worker_loop(void *arg) {
    LcnTaskQueue *q = (LcnTaskQueue *)arg;

    for (;;) {
        /* Lock the queue and wait for work */
        pthread_mutex_lock(&q->mutex);

        while (!q->head && !q->shutdown) {
            pthread_cond_wait(&q->cond, &q->mutex);
        }

        if (q->shutdown && !q->head) {
            /* No more tasks and we are shutting down */
            pthread_mutex_unlock(&q->mutex);
            break;
        }

        LcnTaskHandle *task = queue_pop(q);
        pthread_mutex_unlock(&q->mutex);

        if (!task) continue;  /* spurious wakeup with no task */

        /* Execute the task */
        void *result = task->fn(task->arg);

        /* Mark complete and wake any thread waiting in lcn_await_task */
        pthread_mutex_lock(&task->mutex);
        task->result    = result;
        task->completed = true;
        pthread_cond_signal(&task->cond);
        pthread_mutex_unlock(&task->mutex);
    }

    return NULL;
}

/* ════════════════════════════════════════════════
 * Resolve worker count
 * ════════════════════════════════════════════════ */

static int resolve_worker_count(int requested) {
    if (requested > 0) return requested;

    const char *env = getenv("LCN_WORKERS");
    if (env && env[0] != '\0') {
        int n = atoi(env);
        if (n > 0) return n;
    }

    return LCN_DEFAULT_WORKERS;
}

/* ════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════ */

void lcn_threadpool_init(int num_workers) {
    pthread_mutex_lock(&g_init_mutex);

    if (g_pool.initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return;  /* already initialized */
    }

    int n = resolve_worker_count(num_workers);

    /* Initialize queue */
    g_pool.queue.head     = NULL;
    g_pool.queue.tail     = NULL;
    g_pool.queue.shutdown = false;
    pthread_mutex_init(&g_pool.queue.mutex, NULL);
    pthread_cond_init(&g_pool.queue.cond, NULL);

    /* Spawn worker threads */
    g_pool.num_workers = n;
    g_pool.workers     = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)n);
    if (!g_pool.workers) {
        fprintf(stderr, "lcn_threads: out of memory\n");
        abort();
    }

    for (int i = 0; i < n; i++) {
        int rc = pthread_create(&g_pool.workers[i], NULL, worker_loop, &g_pool.queue);
        if (rc != 0) {
            fprintf(stderr, "lcn_threads: pthread_create failed (%d)\n", rc);
            abort();
        }
    }

    g_pool.initialized = true;
    pthread_mutex_unlock(&g_init_mutex);
}

LcnTaskHandle *lcn_spawn_task(LcnTaskFn fn, void *arg) {
    /* Auto-init if not already initialized */
    if (!g_pool.initialized) {
        lcn_threadpool_init(0);
    }

    LcnTaskHandle *task = (LcnTaskHandle *)malloc(sizeof(LcnTaskHandle));
    if (!task) {
        fprintf(stderr, "lcn_threads: out of memory\n");
        abort();
    }

    task->fn        = fn;
    task->arg       = arg;
    task->result    = NULL;
    task->completed = false;
    pthread_mutex_init(&task->mutex, NULL);
    pthread_cond_init(&task->cond, NULL);

    /* Enqueue and wake a worker */
    pthread_mutex_lock(&g_pool.queue.mutex);
    queue_push(&g_pool.queue, task);
    pthread_cond_signal(&g_pool.queue.cond);
    pthread_mutex_unlock(&g_pool.queue.mutex);

    return task;
}

void *lcn_await_task(LcnTaskHandle *handle) {
    if (!handle) return NULL;

    pthread_mutex_lock(&handle->mutex);
    while (!handle->completed) {
        pthread_cond_wait(&handle->cond, &handle->mutex);
    }
    void *result = handle->result;
    pthread_mutex_unlock(&handle->mutex);

    /* Cleanup the handle */
    pthread_mutex_destroy(&handle->mutex);
    pthread_cond_destroy(&handle->cond);
    free(handle);

    return result;
}

void lcn_threadpool_shutdown(void) {
    pthread_mutex_lock(&g_init_mutex);

    if (!g_pool.initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return;
    }

    /* Signal shutdown: set flag and wake all workers */
    pthread_mutex_lock(&g_pool.queue.mutex);
    g_pool.queue.shutdown = true;
    pthread_cond_broadcast(&g_pool.queue.cond);
    pthread_mutex_unlock(&g_pool.queue.mutex);

    /* Join all worker threads */
    for (int i = 0; i < g_pool.num_workers; i++) {
        pthread_join(g_pool.workers[i], NULL);
    }

    /* Drain any remaining tasks in the queue (shouldn't happen, but be safe) */
    pthread_mutex_lock(&g_pool.queue.mutex);
    while (g_pool.queue.head) {
        LcnTaskHandle *task = queue_pop(&g_pool.queue);
        if (task) {
            pthread_mutex_destroy(&task->mutex);
            pthread_cond_destroy(&task->cond);
            free(task);
        }
    }
    pthread_mutex_unlock(&g_pool.queue.mutex);

    /* Destroy queue synchronization primitives */
    pthread_mutex_destroy(&g_pool.queue.mutex);
    pthread_cond_destroy(&g_pool.queue.cond);

    /* Free workers array */
    free(g_pool.workers);
    g_pool.workers     = NULL;
    g_pool.num_workers = 0;
    g_pool.initialized = false;

    pthread_mutex_unlock(&g_init_mutex);
}

bool lcn_threadpool_active(void) {
    return g_pool.initialized;
}

/* ════════════════════════════════════════════════
 * Structured Concurrency — TaskGroup
 *
 * A TaskGroup collects spawned tasks and guarantees they all
 * complete before await_all returns.  Tasks cannot escape the
 * group scope.
 * ════════════════════════════════════════════════ */

#define LCN_TG_INITIAL_CAP 8

struct LcnTaskGroup {
    LcnTaskHandle **tasks;
    int             count;
    int             capacity;
};

LcnTaskGroup *lcn_task_group_new(void) {
    LcnTaskGroup *tg = (LcnTaskGroup *)malloc(sizeof(LcnTaskGroup));
    if (!tg) {
        fprintf(stderr, "lcn_task_group: out of memory\n");
        abort();
    }
    tg->capacity = LCN_TG_INITIAL_CAP;
    tg->count    = 0;
    tg->tasks    = (LcnTaskHandle **)malloc(sizeof(LcnTaskHandle *) * (size_t)tg->capacity);
    if (!tg->tasks) {
        fprintf(stderr, "lcn_task_group: out of memory\n");
        abort();
    }
    return tg;
}

void lcn_task_group_spawn(LcnTaskGroup *tg, LcnTaskFn fn, void *arg) {
    if (!tg) return;

    /* Grow if needed */
    if (tg->count >= tg->capacity) {
        tg->capacity *= 2;
        tg->tasks = (LcnTaskHandle **)realloc(tg->tasks,
                        sizeof(LcnTaskHandle *) * (size_t)tg->capacity);
        if (!tg->tasks) {
            fprintf(stderr, "lcn_task_group: out of memory\n");
            abort();
        }
    }

    /* Submit to thread pool and record handle */
    LcnTaskHandle *handle = lcn_spawn_task(fn, arg);
    tg->tasks[tg->count++] = handle;
}

void **lcn_task_group_await_all(LcnTaskGroup *tg) {
    if (!tg || tg->count == 0) return NULL;

    void **results = (void **)malloc(sizeof(void *) * (size_t)tg->count);
    if (!results) {
        fprintf(stderr, "lcn_task_group: out of memory\n");
        abort();
    }

    /* Wait for every task in order; collect results */
    for (int i = 0; i < tg->count; i++) {
        results[i] = lcn_await_task(tg->tasks[i]);
        tg->tasks[i] = NULL;  /* handle freed by lcn_await_task */
    }

    return results;
}

void lcn_task_group_free(LcnTaskGroup *tg) {
    if (!tg) return;

    /* Safety: await any tasks that were not yet awaited (scope guarantee) */
    for (int i = 0; i < tg->count; i++) {
        if (tg->tasks[i] != NULL) {
            lcn_await_task(tg->tasks[i]);
            tg->tasks[i] = NULL;
        }
    }

    free(tg->tasks);
    free(tg);
}
