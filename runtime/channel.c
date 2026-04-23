/*
 * Limceron Channel — Implementation
 *
 * Thread-safe, typed ring buffer with pthread mutex + condvars.
 * Transports any data type via memcpy of elem_size bytes per slot.
 *
 * Unbuffered channels (capacity=0) are implemented as capacity=1
 * with synchronous handoff semantics: the sender blocks until the
 * receiver has consumed the item.
 *
 * C99 + pthreads.
 */

#include "channel.h"
#include "event.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ============================================================
 * Channel structure
 * ============================================================ */

struct LcnChannel {
    void            *buffer;        /* ring buffer: capacity * elem_size bytes */
    size_t           elem_size;     /* size of each element */
    int              capacity;      /* max items (>= 1) */
    int              head;          /* read position */
    int              tail;          /* write position */
    int              count;         /* current items in buffer */
    bool             closed;
    bool             unbuffered;    /* true if user requested capacity=0 */

    pthread_mutex_t  mutex;
    pthread_cond_t   not_empty;     /* signaled when item added */
    pthread_cond_t   not_full;      /* signaled when item removed */
};

/* ============================================================
 * Internal helpers
 * ============================================================ */

/* Pointer to the slot at index i within the ring buffer. */
static inline void *slot_ptr(LcnChannel *ch, int i)
{
    return (char *)ch->buffer + (size_t)i * ch->elem_size;
}

/* ============================================================
 * Create / Free
 * ============================================================ */

LcnChannel *lcn_channel_new(int capacity, size_t elem_size)
{
    if (elem_size == 0) return NULL;

    LcnChannel *ch = (LcnChannel *)calloc(1, sizeof(LcnChannel));
    if (!ch) return NULL;

    ch->elem_size  = elem_size;
    ch->unbuffered = (capacity <= 0);
    ch->capacity   = (capacity <= 0) ? 1 : capacity;
    ch->head       = 0;
    ch->tail       = 0;
    ch->count      = 0;
    ch->closed     = false;

    ch->buffer = malloc((size_t)ch->capacity * elem_size);
    if (!ch->buffer) {
        free(ch);
        return NULL;
    }

    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);

    return ch;
}

void lcn_channel_free(LcnChannel *ch)
{
    if (!ch) return;

    pthread_mutex_destroy(&ch->mutex);
    pthread_cond_destroy(&ch->not_empty);
    pthread_cond_destroy(&ch->not_full);

    free(ch->buffer);
    free(ch);
}

/* ============================================================
 * Send
 * ============================================================ */

bool lcn_channel_send(LcnChannel *ch, const void *data)
{
    if (!ch || !data) return false;

    pthread_mutex_lock(&ch->mutex);

    /* Block while buffer is full and channel is open */
    while (ch->count == ch->capacity && !ch->closed) {
        pthread_cond_wait(&ch->not_full, &ch->mutex);
    }

    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return false;
    }

    /* Copy data into the tail slot */
    memcpy(slot_ptr(ch, ch->tail), data, ch->elem_size);
    ch->tail = (ch->tail + 1) % ch->capacity;
    ch->count++;

    /* Wake a blocked receiver */
    pthread_cond_signal(&ch->not_empty);

    /* Unbuffered: wait until the item has been consumed (synchronous handoff) */
    if (ch->unbuffered) {
        while (ch->count > 0 && !ch->closed) {
            pthread_cond_wait(&ch->not_full, &ch->mutex);
        }
    }

    pthread_mutex_unlock(&ch->mutex);

    /* lcn_emit_channel_send("", ""); — dashboard event (linked when available) */
    return true;
}

/* ============================================================
 * Receive
 * ============================================================ */

bool lcn_channel_recv(LcnChannel *ch, void *data)
{
    if (!ch || !data) return false;

    pthread_mutex_lock(&ch->mutex);

    /* Block while buffer is empty and channel is open */
    while (ch->count == 0 && !ch->closed) {
        pthread_cond_wait(&ch->not_empty, &ch->mutex);
    }

    /* Closed and drained */
    if (ch->count == 0 && ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return false;
    }

    /* Copy data out of the head slot */
    memcpy(data, slot_ptr(ch, ch->head), ch->elem_size);
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;

    /* Wake a blocked sender */
    pthread_cond_signal(&ch->not_full);

    pthread_mutex_unlock(&ch->mutex);

    /* lcn_emit_channel_recv("", ""); — dashboard event (linked when available) */
    return true;
}

/* ============================================================
 * Try Receive (non-blocking)
 * ============================================================ */

bool lcn_channel_try_recv(LcnChannel *ch, void *data)
{
    if (!ch || !data) return false;

    pthread_mutex_lock(&ch->mutex);

    if (ch->count == 0) {
        pthread_mutex_unlock(&ch->mutex);
        return false;
    }

    memcpy(data, slot_ptr(ch, ch->head), ch->elem_size);
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;

    pthread_cond_signal(&ch->not_full);

    pthread_mutex_unlock(&ch->mutex);

    /* lcn_emit_channel_recv("", ""); — dashboard event (linked when available) */
    return true;
}

/* ============================================================
 * Close / Query
 * ============================================================ */

void lcn_channel_close(LcnChannel *ch)
{
    if (!ch) return;

    pthread_mutex_lock(&ch->mutex);
    ch->closed = true;
    /* Wake ALL waiters so they can observe the closed state */
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mutex);
}

bool lcn_channel_is_closed(LcnChannel *ch)
{
    if (!ch) return true;

    pthread_mutex_lock(&ch->mutex);
    bool closed = ch->closed;
    pthread_mutex_unlock(&ch->mutex);

    return closed;
}

int lcn_channel_len(LcnChannel *ch)
{
    if (!ch) return 0;

    pthread_mutex_lock(&ch->mutex);
    int len = ch->count;
    pthread_mutex_unlock(&ch->mutex);

    return len;
}
