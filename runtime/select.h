/*
 * Limceron Runtime — Select Multiplexing
 *
 * Polls multiple channels and returns when one has data ready.
 * Stage 1 pragmatic implementation: simple polling loop with 1ms sleep.
 * Fine for <100 channels.
 *
 * C99 + pthreads.
 */

#ifndef LCN_SELECT_H
#define LCN_SELECT_H

#include "channel.h"

/* Poll multiple channels. Returns index of first channel with data ready.
 * Returns -1 if all channels are closed and empty.
 * timeout_ms = 0 means no timeout (block forever).
 * timeout_ms > 0 means return -2 on timeout. */
int lcn_select(LcnChannel **channels, int n, int timeout_ms);

#endif /* LCN_SELECT_H */
