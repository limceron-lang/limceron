/*
 * Limceron Runtime — Select Multiplexing Implementation
 *
 * Simple polling loop: try_recv on each channel in round-robin.
 * If none ready, sleep 1ms and retry.  If timeout exceeded, return -2.
 * If all channels are closed and empty, return -1.
 *
 * This is a Stage 1 pragmatic approach — not epoll/kqueue.
 * A 1ms poll loop is acceptable for <100 channels.
 *
 * Compile: cc -std=c99 -O2 -Wall -c select.c -o select.o
 */

#define _POSIX_C_SOURCE 200112L

#include "select.h"

#include <stdlib.h>
#include <time.h>

/* ════════════════════════════════════════════════
 * Platform-specific millisecond sleep
 * ════════════════════════════════════════════════ */

#if defined(_WIN32)
#include <windows.h>
static void sleep_ms(int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

/* ════════════════════════════════════════════════
 * Monotonic clock for timeout tracking
 * ════════════════════════════════════════════════ */

static long elapsed_ms(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long sec  = now.tv_sec  - start->tv_sec;
    long nsec = now.tv_nsec - start->tv_nsec;
    return sec * 1000L + nsec / 1000000L;
}

/* ════════════════════════════════════════════════
 * lcn_select — poll multiple channels
 * ════════════════════════════════════════════════ */

int lcn_select(LcnChannel **channels, int n, int timeout_ms)
{
    if (!channels || n <= 0) return -1;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Allocate a small scratch buffer to track per-channel data.
     * We need a temporary buffer for try_recv.  Since we don't know
     * elem_size here, we use a dummy 1-byte buffer — try_recv will
     * only check readiness and not actually dequeue when we pass NULL.
     *
     * Actually, try_recv copies data out.  We can't peek without
     * consuming.  So the strategy is: try_recv into a temp buffer,
     * and if it succeeds, that data is consumed.  The caller must
     * then call lcn_channel_recv on the returned index to get the data.
     *
     * Wait — that would consume the data.  We need a different approach.
     *
     * Better: use lcn_channel_len() to check if data is available,
     * which is non-destructive.  Then the caller does the actual recv.
     */

    int offset = 0;  /* round-robin start for fairness */

    for (;;) {
        int all_closed = 1;

        for (int i = 0; i < n; i++) {
            int idx = (i + offset) % n;
            LcnChannel *ch = channels[idx];
            if (!ch) continue;

            if (!lcn_channel_is_closed(ch)) {
                all_closed = 0;
            }

            /* Check if channel has data ready (non-destructive) */
            if (lcn_channel_len(ch) > 0) {
                return idx;
            }
        }

        /* All channels closed and empty */
        if (all_closed) return -1;

        /* Check timeout */
        if (timeout_ms > 0 && elapsed_ms(&start) >= timeout_ms) {
            return -2;
        }

        /* Sleep 1ms before retrying */
        sleep_ms(1);

        /* Rotate round-robin offset for fairness */
        offset = (offset + 1) % n;
    }
}
