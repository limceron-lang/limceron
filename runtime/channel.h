/*
 * Limceron Channel — Typed, Thread-Safe Message Passing
 *
 * Buffered channels with mutex + condvar synchronization.
 * Transports ANY data as void* via memcpy of elem_size bytes.
 * Key use case: transporting LcnLlmOutput structs (with entropy/confidence).
 *
 * C99 + pthreads.
 */

#ifndef LCN_CHANNEL_H
#define LCN_CHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Opaque channel handle (forward-declared in lcn_runtime.h) */
#ifndef LCN_CHANNEL_TYPEDEF
#define LCN_CHANNEL_TYPEDEF
typedef struct LcnChannel LcnChannel;
#endif

/* Create a new buffered channel.
 * capacity = 0 means unbuffered (synchronous send/recv).
 * elem_size = size of each element (e.g., sizeof(LcnLlmOutput)).
 * For string channels, elem_size = sizeof(char*). */
LcnChannel *lcn_channel_new(int capacity, size_t elem_size);

/* Send a value to the channel. Blocks if buffer is full.
 * data is copied into the channel buffer (memcpy of elem_size bytes).
 * Returns false if channel is closed. */
bool lcn_channel_send(LcnChannel *ch, const void *data);

/* Receive a value from the channel. Blocks if buffer is empty.
 * data is copied out of the channel buffer (memcpy of elem_size bytes).
 * Returns false if channel is closed and empty. */
bool lcn_channel_recv(LcnChannel *ch, void *data);

/* Try to receive without blocking. Returns false if empty or closed. */
bool lcn_channel_try_recv(LcnChannel *ch, void *data);

/* Close the channel. Pending sends will fail, pending recvs drain buffer. */
void lcn_channel_close(LcnChannel *ch);

/* Check if channel is closed. */
bool lcn_channel_is_closed(LcnChannel *ch);

/* Get number of items currently in the buffer. */
int lcn_channel_len(LcnChannel *ch);

/* Free the channel. Must be closed first. */
void lcn_channel_free(LcnChannel *ch);

#endif /* LCN_CHANNEL_H */
