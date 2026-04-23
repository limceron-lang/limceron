/* ============================================================
 * Limceron Runtime — Hurd-inspired Capability Delegation
 * ============================================================ */

#include "delegation.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static int64_t delegation_now_ms(void) {
    return (int64_t)GetTickCount64();
}
#else
#include <sys/time.h>
static int64_t delegation_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
#endif

/* ── Create ──────────────────────────────────────────────── */

LcnDelegation *lcn_delegate_new(LcnCapability parent_caps,
                                LcnCapability requested,
                                int64_t       revoke_after_ms) {
    LcnDelegation *d = (LcnDelegation *)malloc(sizeof(LcnDelegation));
    if (!d) return NULL;

    /* Core invariant: child caps = requested ∩ parent_caps */
    d->caps            = requested & parent_caps;
    d->original        = d->caps;
    d->revoke_after_ms = revoke_after_ms;
    d->created_at_ms   = delegation_now_ms();
    return d;
}

/* ── Revoke ──────────────────────────────────────────────── */

void lcn_delegate_revoke(LcnDelegation *d, LcnCapability cap) {
    if (!d) return;
    d->caps &= ~cap;
}

void lcn_delegate_revoke_all(LcnDelegation *d) {
    if (!d) return;
    d->caps = 0;
}

/* ── Query ───────────────────────────────────────────────── */

bool lcn_delegate_check_timeout(LcnDelegation *d) {
    if (!d) return true;
    if (d->revoke_after_ms <= 0) return false;     /* no timeout set */
    int64_t elapsed = delegation_now_ms() - d->created_at_ms;
    if (elapsed >= d->revoke_after_ms) {
        d->caps = 0;   /* auto-revoke on expiry */
        return true;
    }
    return false;
}

bool lcn_delegate_has(LcnDelegation *d, LcnCapability cap) {
    if (!d) return false;
    /* Enforce timeout first */
    if (lcn_delegate_check_timeout(d)) return false;
    return (d->caps & cap) != 0;
}

/* ── Cleanup ─────────────────────────────────────────────── */

void lcn_delegate_free(LcnDelegation *d) {
    free(d);
}
