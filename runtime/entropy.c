/*
 * Limceron Runtime — Entropy Tracker Implementation
 *
 * Thread-safe ring buffer for tracking classification entropy
 * and enforcing entropy budgets at runtime.
 */

#include "entropy.h"
#include "drift.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <sys/time.h>

/* ════════════════════════════════════════════════
 * Internal Structure
 * ════════════════════════════════════════════════ */

struct LcnEntropyTracker {
    LcnEntropyEntry *ring;       /* ring buffer of entries */
    int              capacity;   /* max entries in ring */
    int              head;       /* oldest entry index */
    int              tail;       /* next write index */
    int              count;      /* current number of entries */
    int              total;      /* total entries ever recorded */
    int              num_categories; /* number of classification categories */
    double          *baseline;   /* baseline distribution (first window) */
    bool             baseline_set;
    pthread_mutex_t  lock;

    /* Static buffer for budget violation messages */
    char             err_buf[256];
};

/* ════════════════════════════════════════════════
 * Helpers
 * ════════════════════════════════════════════════ */

static int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

/* Clamp window to actual count */
static int effective_window(LcnEntropyTracker *t, int window) {
    if (window <= 0 || window > t->count) return t->count;
    return window;
}

/* Get ring index for the i-th most recent entry (0 = most recent) */
static int recent_index(LcnEntropyTracker *t, int i) {
    int idx = t->tail - 1 - i;
    if (idx < 0) idx += t->capacity;
    return idx;
}

/* ════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════ */

LcnEntropyTracker *lcn_entropy_tracker_new(int capacity, int num_categories) {
    if (capacity <= 0) capacity = 100;
    if (num_categories <= 0) num_categories = 1;

    LcnEntropyTracker *t = (LcnEntropyTracker *)calloc(1, sizeof(LcnEntropyTracker));
    if (!t) return NULL;

    t->ring = (LcnEntropyEntry *)calloc((size_t)capacity, sizeof(LcnEntropyEntry));
    if (!t->ring) { free(t); return NULL; }

    t->baseline = (double *)calloc((size_t)num_categories, sizeof(double));
    if (!t->baseline) { free(t->ring); free(t); return NULL; }

    t->capacity = capacity;
    t->num_categories = num_categories;
    t->head = 0;
    t->tail = 0;
    t->count = 0;
    t->total = 0;
    t->baseline_set = false;

    pthread_mutex_init(&t->lock, NULL);
    return t;
}

void lcn_entropy_record(LcnEntropyTracker *tracker, double confidence,
                         double entropy, int category_id) {
    if (!tracker) return;

    pthread_mutex_lock(&tracker->lock);

    LcnEntropyEntry *e = &tracker->ring[tracker->tail];
    e->confidence = confidence;
    e->entropy = entropy;
    e->category_id = category_id;
    e->timestamp_ms = now_ms();

    tracker->tail = (tracker->tail + 1) % tracker->capacity;
    if (tracker->count < tracker->capacity) {
        tracker->count++;
    } else {
        /* Ring is full — advance head (overwrite oldest) */
        tracker->head = (tracker->head + 1) % tracker->capacity;
    }
    tracker->total++;

    pthread_mutex_unlock(&tracker->lock);
}

double lcn_entropy_avg_confidence(LcnEntropyTracker *tracker, int window) {
    if (!tracker) return 0.0;

    pthread_mutex_lock(&tracker->lock);

    int w = effective_window(tracker, window);
    if (w == 0) {
        pthread_mutex_unlock(&tracker->lock);
        return 0.0;
    }

    double sum = 0.0;
    for (int i = 0; i < w; i++) {
        int idx = recent_index(tracker, i);
        sum += tracker->ring[idx].confidence;
    }

    pthread_mutex_unlock(&tracker->lock);
    return sum / (double)w;
}

double lcn_entropy_avg_entropy(LcnEntropyTracker *tracker, int window) {
    if (!tracker) return 0.0;

    pthread_mutex_lock(&tracker->lock);

    int w = effective_window(tracker, window);
    if (w == 0) {
        pthread_mutex_unlock(&tracker->lock);
        return 0.0;
    }

    double sum = 0.0;
    for (int i = 0; i < w; i++) {
        int idx = recent_index(tracker, i);
        sum += tracker->ring[idx].entropy;
    }

    pthread_mutex_unlock(&tracker->lock);
    return sum / (double)w;
}

double lcn_entropy_low_confidence_pct(LcnEntropyTracker *tracker, int window,
                                       double threshold) {
    if (!tracker) return 0.0;

    pthread_mutex_lock(&tracker->lock);

    int w = effective_window(tracker, window);
    if (w == 0) {
        pthread_mutex_unlock(&tracker->lock);
        return 0.0;
    }

    int low_count = 0;
    for (int i = 0; i < w; i++) {
        int idx = recent_index(tracker, i);
        if (tracker->ring[idx].confidence < threshold) {
            low_count++;
        }
    }

    pthread_mutex_unlock(&tracker->lock);
    return (double)low_count / (double)w;
}

void lcn_entropy_distribution(LcnEntropyTracker *tracker, int window,
                               double *dist_out, int num_categories) {
    if (!tracker || !dist_out) return;

    pthread_mutex_lock(&tracker->lock);

    int w = effective_window(tracker, window);
    int nc = num_categories < tracker->num_categories
             ? num_categories : tracker->num_categories;

    /* Zero the output */
    for (int i = 0; i < num_categories; i++) dist_out[i] = 0.0;

    if (w == 0) {
        pthread_mutex_unlock(&tracker->lock);
        return;
    }

    /* Count occurrences */
    for (int i = 0; i < w; i++) {
        int idx = recent_index(tracker, i);
        int cat = tracker->ring[idx].category_id;
        if (cat >= 0 && cat < nc) {
            dist_out[cat] += 1.0;
        }
    }

    /* Normalize to proportions */
    for (int i = 0; i < nc; i++) {
        dist_out[i] /= (double)w;
    }

    pthread_mutex_unlock(&tracker->lock);
}

const char *lcn_entropy_check_budget(LcnEntropyTracker *tracker,
                                      const LcnEntropyBudget *budget) {
    if (!tracker || !budget) return NULL;

    /* Check average entropy */
    double avg_ent = lcn_entropy_avg_entropy(tracker, tracker->count);
    if (avg_ent > budget->max_avg_entropy) {
        snprintf(tracker->err_buf, sizeof(tracker->err_buf),
                 "entropy budget exceeded: avg_entropy=%.3f > max=%.3f",
                 avg_ent, budget->max_avg_entropy);
        return tracker->err_buf;
    }

    /* Check low-confidence percentage */
    double threshold = budget->low_confidence_threshold > 0.0
                       ? budget->low_confidence_threshold : 0.5;
    double low_pct = lcn_entropy_low_confidence_pct(tracker, tracker->count, threshold);
    if (low_pct > budget->max_low_confidence) {
        snprintf(tracker->err_buf, sizeof(tracker->err_buf),
                 "entropy budget exceeded: low_confidence_pct=%.3f > max=%.3f",
                 low_pct, budget->max_low_confidence);
        return tracker->err_buf;
    }

    /* Check drift (KL-divergence from baseline) */
    if (budget->max_drift > 0.0 && tracker->num_categories > 1) {
        int nc = tracker->num_categories;
        double *current = (double *)calloc((size_t)nc, sizeof(double));
        if (current) {
            lcn_entropy_distribution(tracker, tracker->count, current, nc);

            /* Set baseline on first check */
            pthread_mutex_lock(&tracker->lock);
            if (!tracker->baseline_set) {
                memcpy(tracker->baseline, current, (size_t)nc * sizeof(double));
                tracker->baseline_set = true;
            }
            pthread_mutex_unlock(&tracker->lock);

            double drift = lcn_drift(current, tracker->baseline, nc);
            free(current);

            if (drift > budget->max_drift) {
                snprintf(tracker->err_buf, sizeof(tracker->err_buf),
                         "entropy budget exceeded: drift=%.3f > max=%.3f",
                         drift, budget->max_drift);
                return tracker->err_buf;
            }
        }
    }

    return NULL; /* all checks passed */
}

int lcn_entropy_count(LcnEntropyTracker *tracker) {
    if (!tracker) return 0;
    pthread_mutex_lock(&tracker->lock);
    int c = tracker->total;
    pthread_mutex_unlock(&tracker->lock);
    return c;
}

void lcn_entropy_tracker_free(LcnEntropyTracker *tracker) {
    if (!tracker) return;
    pthread_mutex_destroy(&tracker->lock);
    free(tracker->baseline);
    free(tracker->ring);
    free(tracker);
}
