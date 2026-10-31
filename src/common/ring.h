#pragma once
// Pure slot-selection logic of the frame ring in protocol.h, shared by the driver
// (producer) and the unit tests. The compositor side lives in FrameSource.
//
// Invariants: latest is the newest complete buffer (or -1), reader is the buffer the
// consumer is copying out of (or -1). The producer must never write either of them:
// overwriting latest would tear the frame the consumer is about to take, overwriting
// reader would tear the copy in flight. With SD_BUFFERS >= 3 one slot always remains.

#include "protocol.h"

// Returns the slot the producer may write next, or -1 if the inputs are out of range
// (shared memory is writable by other processes; never trust it blindly).
inline int SdPickWriteSlot(long latest, long reader)
{
    if (latest < -1 || latest >= SD_BUFFERS || reader < -1 || reader >= SD_BUFFERS) return -1;
    for (int w = 0; w < SD_BUFFERS; w++)
        if (w != latest && w != reader) return w;
    return -1; // unreachable for SD_BUFFERS >= 3 with in-range inputs
}

// Reads a 64-bit shared field atomically. A plain volatile read of a LONG64 is only
// de-facto atomic on x64; this is correct by construction on every platform.
inline long long SdReadI64(volatile long long* p)
{
    return InterlockedCompareExchange64(p, 0, 0);
}
