/*
 * Limceron Runtime — Drift Detection
 *
 * Statistical divergence measures between probability distributions.
 * Used to detect when an agent's classification behavior has shifted
 * away from its baseline (concept drift).
 *
 * All functions are pure — no state, no threading.
 */

#ifndef LCN_DRIFT_H
#define LCN_DRIFT_H

/* Compute KL-divergence: D_KL(P || Q) = sum(P[i] * log(P[i] / Q[i]))
 * P = current distribution, Q = baseline
 * Both are arrays of n doubles that sum to 1.0.
 * Returns value >= 0. Higher = more drift. */
double lcn_kl_divergence(const double *P, const double *Q, int n);

/* Symmetric KL-divergence: (D_KL(P||Q) + D_KL(Q||P)) / 2 */
double lcn_drift(const double *current, const double *baseline, int n);

/* Jensen-Shannon divergence (bounded [0, ln(2)], more stable than KL) */
double lcn_js_divergence(const double *P, const double *Q, int n);

#endif /* LCN_DRIFT_H */
