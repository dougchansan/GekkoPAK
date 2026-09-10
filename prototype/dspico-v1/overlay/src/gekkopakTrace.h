// Cartridge-side event ring for the GekkoPAK transport.
//
// The RP2040 is the device under test and sees every transaction, so it is the
// right place to observe a run from. What it has lacked is a way to say what it
// saw: results reached the outside world only by way of the DS screen and a
// photograph, which cannot show ordering, cannot show timing, and cannot show
// anything about a transaction the console never noticed had failed.
//
// This is that channel's producer half. gekkopakLink.cpp drains it to a host
// over the RP2040's own USB port -- deliberately not over the cartridge bus,
// which is the thing being measured.
//
// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------
//
// Records are written from the cartridge IRQ, so gpk_trace() must cost almost
// nothing and must never block, allocate, or touch flash. It is a bounds check,
// a handful of stores and a pointer bump, in RAM, and it drops rather than
// waits when the ring is full -- a dropped record is a reported fact, whereas a
// stalled IRQ is a corrupted data phase.
//
// The mask defaults to zero. Tracing is opt-in, per category, from the host, so
// a timing campaign runs against exactly the code path it is measuring unless
// it is explicitly asked to do otherwise.
//
// Producer and consumer both run on core0 -- the IRQ preempts the main loop --
// so the single-producer/single-consumer discipline holds with the producer
// owning `write` and the consumer owning `read`, and no lock is required.

#pragma once

#include "common.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

// RAM placement for anything on the cartridge IRQ path. See the long comment in
// gekkopakNtr.cpp: these must not execute from XIP flash, and must not go in
// SCRATCH_Y, which core0's stack shares.
#ifndef GEKKOPAK_IRQ_FN
#ifdef GEKKOPAK_HOST_SHIM
#define GEKKOPAK_IRQ_FN(name) name
#else
#define GEKKOPAK_IRQ_FN(name) __not_in_flash_func(name)
#endif
#endif

// Event kinds. The mask bit for a kind is 1u << kind, so kinds stay below 32.
enum {
    GPK_TRACE_RESET       = 1,  // device reset; value = local pool bytes
    GPK_TRACE_WRITE_REG   = 2,  // F0: index, value
    GPK_TRACE_EXEC        = 3,  // F1: index, value, aux = result
    GPK_TRACE_READ_REG    = 4,  // F2: index, value returned
    GPK_TRACE_PAYLOAD     = 5,  // F3: index, word
    GPK_TRACE_BLOCK_IN    = 6,  // F4 cmd1: aux = disposition, value = length word
    GPK_TRACE_BLOCK_PARSE = 7,  // F4 data phase complete: aux = result
    GPK_TRACE_BLOCK_ARM   = 8,  // F5 cmd0: index = selector, aux = source
    GPK_TRACE_BLOCK_ACK   = 9,  // F5 cmd1: aux = result, value = dropped records
    GPK_TRACE_KIND_COUNT  = 10
};

// GPK_TRACE_BLOCK_IN dispositions.
enum {
    GPK_TRACE_BLOCK_REFUSED  = 0,  // handler declined the data phase
    GPK_TRACE_BLOCK_ACCEPTED = 1   // data phase started
};

// GPK_TRACE_BLOCK_ARM sources. Which buffer the DMA was pointed at says more
// than the bytes do: a correct-looking block from the zero source means the
// selector was wrong, not that the transport worked.
enum {
    GPK_TRACE_SOURCE_ZEROES     = 0,
    GPK_TRACE_SOURCE_DIAGNOSTIC = 1,
    GPK_TRACE_SOURCE_COMPLETION = 2
};

// Everything except the per-word F3 traffic, which is voluminous and rarely
// what a question is about.
#define GPK_TRACE_MASK_DEFAULT_ON \
    ((1u << GPK_TRACE_KIND_COUNT) - 2u - (1u << GPK_TRACE_PAYLOAD))

typedef struct {
    u32 time_us;  // free-running microsecond counter at the moment of the event
    u8  kind;
    u8  index;    // command index / register / selector
    u16 aux;      // result code or discriminator, per kind
    u32 value;
} gpk_trace_record_t;

// 256 records is about 40 ms of the densest traffic this transport produces and
// costs 3 KiB of the RP2040's 264 KiB.
#define GPK_TRACE_CAPACITY 256

#ifdef __cplusplus
extern "C" {
#endif

// Empties the ring and clears the drop count. Does not change the mask: a host
// that has asked for a category keeps it across a device reset.
void gpk_trace_reset(void);

void gpk_trace_set_mask(u32 mask);
u32  gpk_trace_mask(void);

// Records one event, if its kind is unmasked. IRQ-path safe.
void GEKKOPAK_IRQ_FN(gpk_trace)(u8 kind, u8 index, u16 aux, u32 value);

// Consumer side. Returns false when the ring is empty.
bool gpk_trace_pop(gpk_trace_record_t* out);
u32  gpk_trace_depth(void);
// Records discarded because the ring was full. A non-zero value means the
// transcript has holes, which a reader has to be told about rather than left to
// infer from a gap in the timestamps.
u32  gpk_trace_dropped(void);

// Microsecond timebase. The firmware reads the hardware timer directly rather
// than calling into the SDK, which lives in flash; the host shim supplies a
// deterministic counter so trace assertions are reproducible.
u32 GEKKOPAK_IRQ_FN(gpk_trace_now)(void);

#ifdef __cplusplus
}
#endif
