/*
 * Limceron Runtime — Entropy Tracker
 *
 * Thread-safe ring buffer that records classification results
 * and computes rolling statistics for entropy-aware agents.
 *
 * Used by the runtime to track LLM output quality over time,
 * detect degradation, and enforce entropy budgets.
 */

#ifndef LCN_ENTROPY_H
#define LCN_ENTROPY_H

#include <stdint.h>
#include <stdbool.h>

/* A single classification entry in the ring buffer */
typedef struct {
    double   confidence;      /* [0.0, 1.0] from Shannon entropy */
    double   entropy;         /* raw Shannon entropy */
    int      category_id;     /* index into category array */
    int64_t  timestamp_ms;    /* when this classification happened */
} LcnEntropyEntry;

/* Entropy budget configuration */
typedef struct {
    double max_avg_entropy;       /* pause if avg entropy > this (e.g., 0.7) */
    double max_low_confidence;    /* pause if % low-confidence > this (e.g., 0.20) */
    double max_drift;             /* pause if KL-divergence > this (e.g., 0.15) */
    double low_confidence_threshold; /* what counts as "low" (default: 0.5) */
} LcnEntropyBudget;

/* Opaque entropy tracker */
typedef struct LcnEntropyTracker LcnEntropyTracker;

/* Create a tracker with given ring buffer capacity */
LcnEntropyTracker *lcn_entropy_tracker_new(int capacity, int num_categories);

/* Record a new classification result */
void lcn_entropy_record(LcnEntropyTracker *tracker, double confidence, double entropy, int category_id);

/* Get statistics over the last N entries (or all if window > count) */
double lcn_entropy_avg_confidence(LcnEntropyTracker *tracker, int window);
double lcn_entropy_avg_entropy(LcnEntropyTracker *tracker, int window);

/* Get percentage of entries below the low-confidence threshold */
double lcn_entropy_low_confidence_pct(LcnEntropyTracker *tracker, int window, double threshold);

/* Get current category distribution as array of doubles (caller provides buffer) */
void lcn_entropy_distribution(LcnEntropyTracker *tracker, int window, double *dist_out, int num_categories);

/* Check entropy budget. Returns NULL if OK, or error string if violated. */
const char *lcn_entropy_check_budget(LcnEntropyTracker *tracker, const LcnEntropyBudget *budget);

/* Get total entries recorded */
int lcn_entropy_count(LcnEntropyTracker *tracker);

/* Free tracker */
void lcn_entropy_tracker_free(LcnEntropyTracker *tracker);

#endif /* LCN_ENTROPY_H */
