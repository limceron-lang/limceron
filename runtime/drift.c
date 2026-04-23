/*
 * Limceron Runtime — Drift Detection Implementation
 *
 * Statistical divergence measures for detecting concept drift
 * in agent classification behavior.
 *
 * All functions are pure — no state, no threading, no allocations.
 */

#include "drift.h"
#include <math.h>
#include <stdlib.h>

/* Small epsilon to avoid log(0) */
#define DRIFT_EPS 1e-10

/* ════════════════════════════════════════════════
 * KL-Divergence
 * ════════════════════════════════════════════════ */

double lcn_kl_divergence(const double *P, const double *Q, int n) {
    double kl = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        double p = P[i] + DRIFT_EPS;
        double q = Q[i] + DRIFT_EPS;
        kl += p * log(p / q);
    }
    /* Clamp to non-negative (numerical noise can produce tiny negatives) */
    return kl > 0.0 ? kl : 0.0;
}

/* ════════════════════════════════════════════════
 * Symmetric KL-Divergence
 * ════════════════════════════════════════════════ */

double lcn_drift(const double *current, const double *baseline, int n) {
    double kl_pq = lcn_kl_divergence(current, baseline, n);
    double kl_qp = lcn_kl_divergence(baseline, current, n);
    return (kl_pq + kl_qp) / 2.0;
}

/* ════════════════════════════════════════════════
 * Jensen-Shannon Divergence
 * ════════════════════════════════════════════════ */

double lcn_js_divergence(const double *P, const double *Q, int n) {
    /* M = (P + Q) / 2, then JS = (KL(P||M) + KL(Q||M)) / 2 */
    /* Stack-allocate M for small n; heap for large */
    double stack_m[64];
    double *M = (n <= 64) ? stack_m : NULL;
    int i;

    if (!M) {
        /* For large category counts, allocate on heap */
        M = (double *)malloc((size_t)n * sizeof(double));
        if (!M) return 0.0;
    }

    for (i = 0; i < n; i++) {
        M[i] = (P[i] + Q[i]) / 2.0;
    }

    double kl_pm = lcn_kl_divergence(P, M, n);
    double kl_qm = lcn_kl_divergence(Q, M, n);

    double js = (kl_pm + kl_qm) / 2.0;

    if (M != stack_m) free(M);

    return js;
}
