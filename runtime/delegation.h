/* ============================================================
 * Limceron Runtime — Hurd-inspired Capability Delegation
 *
 * Supervisors can delegate a subset of their capabilities to
 * spawned workers, and revoke them at runtime.
 *
 * Key invariant: delegated caps are always a subset of the
 * parent's caps.  No escalation is possible.
 * ============================================================ */

#ifndef LCN_DELEGATION_H
#define LCN_DELEGATION_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

#ifndef LCN_CAPABILITY_DEFINED
#define LCN_CAPABILITY_DEFINED
typedef uint64_t LcnCapability;
#endif

/* A delegation handle tracks the capabilities given to a worker. */
typedef struct {
    LcnCapability caps;             /* current capability set            */
    LcnCapability original;         /* what was delegated at creation    */
    int64_t       revoke_after_ms;  /* auto-revoke timeout (0 = never)  */
    int64_t       created_at_ms;    /* timestamp of creation             */
} LcnDelegation;

/* Create a new delegation.  `requested` is intersected with
 * `parent_caps` so the child can never exceed the parent.       */
LcnDelegation *lcn_delegate_new(LcnCapability parent_caps,
                                LcnCapability requested,
                                int64_t       revoke_after_ms);

/* Revoke a single capability from a delegation. */
void lcn_delegate_revoke(LcnDelegation *d, LcnCapability cap);

/* Revoke all capabilities from a delegation. */
void lcn_delegate_revoke_all(LcnDelegation *d);

/* Check whether a delegation still holds a given capability.
 * Also enforces the auto-revoke timeout: if expired, clears
 * all caps before returning false.                              */
bool lcn_delegate_has(LcnDelegation *d, LcnCapability cap);

/* Returns true if the delegation has expired (timeout elapsed). */
bool lcn_delegate_check_timeout(LcnDelegation *d);

/* Free a delegation handle. */
void lcn_delegate_free(LcnDelegation *d);

#endif /* LCN_DELEGATION_H */
