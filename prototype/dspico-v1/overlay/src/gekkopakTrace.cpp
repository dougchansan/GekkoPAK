// Cartridge-side event ring. See gekkopakTrace.h for the contract.

#include "gekkopakTrace.h"

#ifndef GEKKOPAK_HOST_SHIM
#include "hardware/timer.h"
#endif

namespace {

// The producer owns sWrite and the consumer owns sRead, so no lock is needed
// even though the producer is an interrupt that preempts the consumer. Both are
// volatile because each side observes the other's index.
volatile u32 sWrite;
volatile u32 sRead;
volatile u32 sDropped;
u32 sMask;

gpk_trace_record_t sRing[GPK_TRACE_CAPACITY];

} // namespace

extern "C" u32 GEKKOPAK_IRQ_FN(gpk_trace_now)(void) {
#ifdef GEKKOPAK_HOST_SHIM
    // A counter rather than a clock: the host tests assert on ordering and on
    // which events appeared, and a real clock would make them irreproducible.
    static u32 sTick;
    return ++sTick;
#else
    // Read the always-running hardware timer directly. time_us_32() would do
    // the same thing by way of a call into flash, which this path cannot
    // afford; the low word alone is enough, since nothing here measures an
    // interval longer than 71 minutes.
    return timer_hw->timerawl;
#endif
}

extern "C" void gpk_trace_reset(void) {
    sWrite = 0;
    sRead = 0;
    sDropped = 0;
}

extern "C" void gpk_trace_set_mask(u32 mask) { sMask = mask; }

extern "C" u32 gpk_trace_mask(void) { return sMask; }

extern "C" void GEKKOPAK_IRQ_FN(gpk_trace)(u8 kind, u8 index, u16 aux, u32 value) {
    // Masked-off tracing is a load, a shift and a branch. That is the cost this
    // path pays when a timing campaign is running, and it is why the mask
    // defaults to zero rather than to something useful.
    if (!(sMask & (1u << kind))) {
        return;
    }

    const u32 write = sWrite;
    const u32 next = (write + 1u) % GPK_TRACE_CAPACITY;
    if (next == sRead) {
        // Full. Drop the new record rather than overwrite the oldest: the start
        // of a run is what explains the rest of it, and a stalled IRQ would
        // corrupt the very data phase being traced.
        ++sDropped;
        return;
    }

    gpk_trace_record_t* slot = &sRing[write];
    slot->time_us = gpk_trace_now();
    slot->kind = kind;
    slot->index = index;
    slot->aux = aux;
    slot->value = value;

    // Publish only after the record is complete, so a consumer preempted
    // mid-pop never sees a half-written slot.
    __asm__ volatile("" ::: "memory");
    sWrite = next;
}

extern "C" bool gpk_trace_pop(gpk_trace_record_t* out) {
    const u32 read = sRead;
    if (read == sWrite) {
        return false;
    }
    *out = sRing[read];
    __asm__ volatile("" ::: "memory");
    sRead = (read + 1u) % GPK_TRACE_CAPACITY;
    return true;
}

extern "C" u32 gpk_trace_depth(void) {
    return (sWrite - sRead) % GPK_TRACE_CAPACITY;
}

extern "C" u32 gpk_trace_dropped(void) { return sDropped; }
