#include "bench.h"
#include <stdlib.h>
#include <string.h>

const u8 gpkTestPattern[16] = {
    0x44, 0x33, 0x22, 0x11,
    0x88, 0x77, 0x66, 0x55,
    0xDD, 0xCC, 0xBB, 0xAA,
    0x0D, 0xF0, 0xAD, 0x0B,
};

static u8  sBlock[GPK_BLOCK_BYTES] __attribute__((aligned(4)));
static u8  sReadBlock[GPK_BLOCK_BYTES] __attribute__((aligned(4)));
static u8  sPrimeBlock[GPK_BLOCK_BYTES] __attribute__((aligned(4)));
static u32 sSamples[GPK_BENCH_ITERATIONS];

static int cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return (x > y) - (x < y);
}

void gpk_stats_compute(u32 *samples, u32 count, gpk_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (count == 0)
        return;
    qsort(samples, count, sizeof(u32), cmp_u32);
    u64 total = 0;
    for (u32 i = 0; i < count; i++)
        total += samples[i];
    out->samples   = count;
    out->min_us    = samples[0];
    out->max_us    = samples[count - 1];
    out->median_us = samples[count / 2];
    out->mean_us   = (u32)(total / count);
    // Nearest-rank p95.
    u32 rank = (count * 95 + 99) / 100;
    if (rank == 0) rank = 1;
    out->p95_us = samples[rank - 1];
}

// Build one GKD1 descriptor into dst.
static void gpk_build_descriptor(gpk_descriptor_t *dst, u32 sequence, u32 handle,
                                 u32 inputLength, u32 flags, u32 workUnits)
{
    memset(dst, 0, sizeof(*dst));
    dst->magic         = GPK_DESC_MAGIC;
    dst->version       = GPK_BLOCK_VERSION;
    dst->opcode        = GPK_SUBMIT_OPCODE;
    dst->sequence      = sequence;
    dst->flags         = flags;
    dst->input_handle  = handle;
    dst->input_offset  = 0;
    dst->input_length  = inputLength;
    dst->output_length = 16;
    dst->kernel_id     = 1;
    dst->work_units    = workUnits;
    dst->arg0          = 3500; // nominal software_us reference
}

// Stage len bytes into the legacy payload buffer with F3, then commit them into
// allocation handle with the F1 UPLOAD command. Returns the running FNV-1a.
static u32 gpk_upload(u32 handle, const u8 *data, u32 len)
{
    u32 words = (len + 3) / 4;
    for (u32 i = 0; i < words; i++) {
        u32 w = 0;
        u32 remaining = len - i * 4;
        memcpy(&w, data + i * 4, remaining >= 4 ? 4 : remaining);
        gpk_payload_word((u8)i, w);
    }
    gpk_write_reg(GPK_REG_ARG0, handle);
    gpk_write_reg(GPK_REG_ARG1, 0);
    gpk_write_reg(GPK_REG_PAYLOAD_LEN, len);
    gpk_exec(GPK_CMD_UPLOAD);
    if (gpk_read_reg(GPK_REG_RESULT) != GPK_OK)
        return 0;
    return gpk_read_reg(GPK_REG_OUT1);
}

// Drain leftover completions so queue depth starts at zero.
static void gpk_drain_completions(void)
{
    for (int guard = 0; guard < 8 && gpk_event_depth() != 0; guard++)
        gpk_read_block(sReadBlock, GPK_BLOCK_BYTES);
}

// ---------------------------------------------------------------------------
// Stage 1-2: discovery and legacy F0-F3 control validation.
// ---------------------------------------------------------------------------
static bool gpk_stage_discovery(gpk_report_t *r)
{
    r->init_layer    = gpk_transport_init();
    r->latency_read  = gpkLatencyRead;
    r->latency_write = gpkLatencyWrite;
    if (r->init_layer != GPK_LAYER_NONE) {
        r->device_present = false;
        return false;
    }
    u32 result = gpk_hello(&r->protocol, &r->caps, &r->local_bytes, &r->transport);
    r->device_present = (result == GPK_OK) && (r->protocol == GPK_PROTOCOL_V1);
    return r->device_present;
}

static bool gpk_stage_legacy(gpk_report_t *r)
{
    // Deterministic round trip through F0 and F2: the value must survive the
    // big-endian command assembly untouched.
    const u32 probe = 0xDEADBEEFu;
    gpk_write_reg(GPK_REG_ARG0, probe);
    if (gpk_read_reg(GPK_REG_ARG0) != probe) {
        r->legacy_ok = false;
        return false;
    }
    gpk_write_reg(GPK_REG_ARG0, 0x00000001u);
    if (gpk_read_reg(GPK_REG_ARG0) != 0x00000001u) {
        r->legacy_ok = false;
        return false;
    }

    // F3 + UPLOAD must reproduce the reference checksum too, which isolates F3
    // byte ordering from the F4 block path.
    u32 size = 0;
    u32 handle = gpk_alloc(sizeof(gpkTestPattern), &size);
    if (handle == 0) {
        r->legacy_ok = false;
        return false;
    }
    r->legacy_checksum = gpk_upload(handle, gpkTestPattern, sizeof(gpkTestPattern));
    gpk_write_reg(GPK_REG_ARG0, handle);
    gpk_exec(GPK_CMD_FREE);
    r->legacy_ok = (r->legacy_checksum == GPK_EXPECTED_CHECKSUM);
    return r->legacy_ok;
}

// ---------------------------------------------------------------------------
// Stage 3-5: F4 write, event check, F5 read, checksum validation.
// ---------------------------------------------------------------------------
static bool gpk_stage_block_roundtrip(gpk_report_t *r)
{
    gpk_drain_completions();

    u32 size = 0;
    u32 handle = gpk_alloc(sizeof(gpkTestPattern), &size);
    r->block_handle = handle;
    if (handle == 0)
        return false;

    // One descriptor carrying the pattern inline: 64 + 16 = 80 meaningful bytes.
    memset(sBlock, 0, sizeof(sBlock));
    gpk_build_descriptor((gpk_descriptor_t *)sBlock, 1, handle,
                         sizeof(gpkTestPattern), GPK_FLAG_INLINE_INPUT, 100000);
    memcpy(sBlock + GPK_DESCRIPTOR_BYTES, gpkTestPattern, sizeof(gpkTestPattern));

    // Prime before the block write, and retry it once.
    //
    // The first transaction after a pause is dropped on this cartridge - the
    // reason control reads are issued twice. A single F4 following the ALLOC
    // sequence is exactly that case, and a dropped F4 queues no completion,
    // which is what "event depth 0" reported. Priming with a cheap read and
    // resending on an empty queue covers it. Each attempt uses its own sequence
    // number so a completion can be attributed to the attempt that produced it.
    r->event_depth = 0;
    for (u32 attempt = 0; attempt < 2 && r->event_depth == 0; attempt++) {
        ((gpk_descriptor_t *)sBlock)->sequence = 1 + attempt;

        // Prime with a block write, not a register read.
        //
        // The dropped-first-transaction behaviour applies to the F4 here too,
        // and the previous prime was an F2 read - so the real F4 was still the
        // first write-direction transfer after a run of reads. A block whose
        // descriptor region is zeroed is the safe primer: processDescriptorBatch
        // stops at the first zero magic, so it is accepted and discarded and
        // queues no completion.
        //
        // This also matches the symptom. The write loop only emits bytes while
        // DATA_READY is set; a transfer that completes without it ever
        // asserting sends nothing, records no timeout - and timeouts were 0 -
        // and leaves DSpico parsing a zeroed block it quietly drops, which is
        // indistinguishable from "event depth 0".
        memset(sPrimeBlock, 0, sizeof(sPrimeBlock));
        gpk_write_block(sPrimeBlock, GPK_BLOCK_BYTES);
        gpk_write_block(sBlock, GPK_DESCRIPTOR_BYTES + sizeof(gpkTestPattern));
        r->f4_ok = true;
        for (int i = 0; i < 16; i++) {
            r->event_depth = gpk_event_depth();
            if (r->event_depth != 0)
                break;
            swiDelay(500);
        }
    }
    if (r->event_depth == 0) {
        r->f5_ok = false;
        return false;
    }

    memset(sReadBlock, 0, sizeof(sReadBlock));
    gpk_read_block(sReadBlock, GPK_COMPLETION_BYTES);
    // Dump the head of the completion block.
    //
    // F4 now works - event depth reaches 1 and the RP2040 reports descriptors
    // parsed - but the GKC1 record does not validate and checksum reads
    // 01000000 instead of F269B734. The legacy path hashes the same bytes
    // correctly, so the data is arriving misaligned rather than wrong, and
    // 0x01000000 looks like the version field (value 1) sitting a few bytes off.
    // Print the raw head rather than infer the shift: GKC1 should start
    // 47 4B 43 31 01 00 00 00.
    memcpy(r->f5_head, sReadBlock, sizeof(r->f5_head));

    const gpk_completion_t *c = (const gpk_completion_t *)sReadBlock;
    r->f5_ok = (c->magic == GPK_COMP_MAGIC) &&
               (c->version == GPK_BLOCK_VERSION) &&
               (c->status == GPK_OK) &&
               (c->sequence == 1 || c->sequence == 2) &&
               (c->job_handle != 0);
    r->checksum    = c->checksum;
    r->checksum_ok = (c->checksum == GPK_EXPECTED_CHECKSUM);

    gpk_write_reg(GPK_REG_ARG0, handle);
    gpk_exec(GPK_CMD_FREE);
    return r->f5_ok && r->checksum_ok;
}

// ---------------------------------------------------------------------------
// Stage 6: bus benchmarks.
// ---------------------------------------------------------------------------
static void gpk_samples_to_us(void)
{
    for (u32 i = 0; i < GPK_BENCH_ITERATIONS; i++)
        sSamples[i] = gpk_ticks_to_us(sSamples[i]);
}

static void gpk_bench_cmd_latency(gpk_stats_t *out)
{
    // gpk_read_reg() issues two transactions to work around the dropped-first
    // transaction behaviour, so measure the raw command here: this figure must
    // be the cost of one F2, not two.
    for (u32 i = 0; i < GPK_BENCH_WARMUP; i++)
        (void)gpk_cmd_read32(GPK_OP_READ_REG, GPK_REG_ARG0, 0);
    for (u32 i = 0; i < GPK_BENCH_ITERATIONS; i++) {
        u32 t0 = gpk_ticks();
        (void)gpk_cmd_read32(GPK_OP_READ_REG, GPK_REG_ARG0, 0);
        sSamples[i] = gpk_ticks() - t0;
        gpk_tick();
    }
    gpk_samples_to_us();
    gpk_stats_compute(sSamples, GPK_BENCH_ITERATIONS, out);
}

// A zeroed descriptor region makes DSpico accept and discard the block, so this
// measures transport only, with no completion-queue side effects.
static void gpk_bench_f4(gpk_stats_t *out, u32 meaningfulBytes)
{
    memset(sBlock, 0, sizeof(sBlock));
    for (u32 i = 0; i < GPK_BENCH_WARMUP; i++)
        gpk_write_block(sBlock, meaningfulBytes);
    for (u32 i = 0; i < GPK_BENCH_ITERATIONS; i++) {
        u32 t0 = gpk_ticks();
        gpk_write_block(sBlock, meaningfulBytes);
        sSamples[i] = gpk_ticks() - t0;
        gpk_tick();
    }
    gpk_samples_to_us();
    gpk_stats_compute(sSamples, GPK_BENCH_ITERATIONS, out);
}

static void gpk_bench_f5(gpk_stats_t *out)
{
    gpk_drain_completions();
    for (u32 i = 0; i < GPK_BENCH_WARMUP; i++)
        gpk_read_block(sReadBlock, GPK_BLOCK_BYTES);
    for (u32 i = 0; i < GPK_BENCH_ITERATIONS; i++) {
        u32 t0 = gpk_ticks();
        gpk_read_block(sReadBlock, GPK_BLOCK_BYTES);
        sSamples[i] = gpk_ticks() - t0;
        gpk_tick();
    }
    gpk_samples_to_us();
    gpk_stats_compute(sSamples, GPK_BENCH_ITERATIONS, out);
}

static void gpk_bench_rtt(gpk_stats_t *out)
{
    memset(sBlock, 0, sizeof(sBlock));
    for (u32 i = 0; i < GPK_BENCH_ITERATIONS; i++) {
        u32 t0 = gpk_ticks();
        gpk_write_block(sBlock, GPK_BLOCK_BYTES);
        (void)gpk_event_depth();
        gpk_read_block(sReadBlock, GPK_BLOCK_BYTES);
        sSamples[i] = gpk_ticks() - t0;
        gpk_tick();
    }
    gpk_samples_to_us();
    gpk_stats_compute(sSamples, GPK_BENCH_ITERATIONS, out);
}

// Payload-size sweep. The NTR bus only offers 4-byte and 512-byte block sizes,
// so a 32-byte payload is either eight F3 word transactions or one 512-byte F4
// whose meaningful region is 32 bytes. Both are measured; the difference is
// exactly the cost model the simulator needs.
static void gpk_bench_sizes(gpk_report_t *r)
{
    static const u32 kSizes[7] = { 4, 16, 32, 64, 128, 256, 512 };
    r->size_count = 7;
    for (u32 s = 0; s < 7; s++) {
        u32 bytes = kSizes[s];
        u32 words = bytes / 4;
        r->sizes[s].bytes = bytes;

        for (u32 i = 0; i < GPK_BENCH_WARMUP; i++)
            for (u32 w = 0; w < words; w++)
                gpk_payload_word((u8)w, 0x5A5A5A5Au);
        for (u32 i = 0; i < GPK_BENCH_ITERATIONS; i++) {
            u32 t0 = gpk_ticks();
            for (u32 w = 0; w < words; w++)
                gpk_payload_word((u8)w, 0x5A5A5A5Au);
            sSamples[i] = gpk_ticks() - t0;
        gpk_tick();
        }
        gpk_samples_to_us();
        gpk_stats_compute(sSamples, GPK_BENCH_ITERATIONS, &r->sizes[s].word_path);

        gpk_bench_f4(&r->sizes[s].block_path, bytes);
    }
}

// Batch scaling: N descriptors in one 512-byte F4, N completions in one F5.
// Eight 64-byte descriptors exactly fill the block, so batch-8 cannot carry
// inline input and references a pre-uploaded allocation instead.
static bool gpk_bench_batch(gpk_report_t *r)
{
    static const u32 kBatches[4] = { 1, 2, 4, 8 };
    r->batch_count = 4;
    bool all_ok = true;

    u32 size = 0;
    u32 handle = gpk_alloc(sizeof(gpkTestPattern), &size);
    if (handle == 0)
        return false;
    if (gpk_upload(handle, gpkTestPattern, sizeof(gpkTestPattern)) != GPK_EXPECTED_CHECKSUM)
        all_ok = false;

    for (u32 b = 0; b < 4; b++) {
        u32 n = kBatches[b];
        r->batches[b].batch = n;
        gpk_drain_completions();

        memset(sBlock, 0, sizeof(sBlock));
        for (u32 i = 0; i < n; i++) {
            gpk_build_descriptor((gpk_descriptor_t *)(sBlock + i * GPK_DESCRIPTOR_BYTES),
                                 100 + i, handle, sizeof(gpkTestPattern), 0, 100000);
        }
        u32 meaningful = n * GPK_DESCRIPTOR_BYTES;

        u64 total = 0;
        u32 iterations = GPK_BENCH_ITERATIONS / 4;
        u32 completions = 0;
        for (u32 i = 0; i < iterations; i++) {
            gpk_drain_completions();
            u32 t0 = gpk_ticks();
            gpk_write_block(sBlock, meaningful);
            gpk_read_block(sReadBlock, meaningful);
            total += gpk_ticks() - t0;
            gpk_tick();
            if (i == 0) {
                for (u32 c = 0; c < n; c++) {
                    const gpk_completion_t *rec =
                        (const gpk_completion_t *)(sReadBlock + c * GPK_COMPLETION_BYTES);
                    if (rec->magic == GPK_COMP_MAGIC && rec->status == GPK_OK &&
                        rec->checksum == GPK_EXPECTED_CHECKSUM)
                        completions++;
                }
            }
        }
        u32 avg_ticks    = (u32)(total / iterations);
        u32 avg_us_x1000 = (u32)(((u64)avg_ticks * 1000000ull) / 33514ull);
        r->batches[b].us_per_job_x1000   = avg_us_x1000 / n;
        r->batches[b].transactions_x1000 = (2u * 1000u) / n;
        r->batches[b].completions        = completions;
        r->batches[b].ok                 = (completions == n);
        if (!r->batches[b].ok)
            all_ok = false;
    }

    gpk_write_reg(GPK_REG_ARG0, handle);
    gpk_exec(GPK_CMD_FREE);
    return all_ok;
}

static void gpk_compute_bandwidth(gpk_report_t *r)
{
    // Sustained bus bandwidth from the median 512-byte transfer time.
    if (r->f4_latency.median_us)
        r->write_kib_per_s =
            (u32)((512ull * 1000000ull) / ((u64)r->f4_latency.median_us * 1024ull));
    if (r->f5_latency.median_us)
        r->read_kib_per_s =
            (u32)((512ull * 1000000ull) / ((u64)r->f5_latency.median_us * 1024ull));
}

// Try several F4 variants in a single boot.
//
// Each previous hardware round trip tested exactly one guess, which is far too
// slow when a round trip costs a card swap and a photograph. These variants
// differ in the things actually in doubt: the meaningful-byte count, whether a
// primer precedes the write, whether the payload is inline or pre-uploaded, and
// the write latency. The RP2040 counters are sampled either side of each
// attempt so every variant reports its own deltas.
// F5 read sweep.
//
// Every variant re-reads the SAME queued completion. A read whose meaningful
// byte count is zero makes the cartridge skip popCompletion() while still
// running ntrc_beginWrite()/ntrc_dmaToBus(), so sBlockRx keeps its contents and
// the 512-byte read path is exercised in full. That means one queued completion
// is enough for all eight variants, and no variant can starve the ones after it.
u32 gpk_f5_matrix(gpk_f5_variant_t *out)
{
    static const u32 kLatencies[4] = { 4, 16, 32, 63 };
    const u32 savedLatency = gpkLatencyBlockRead;
    const u32 savedPrime   = gpkPrimeBlockRead;

    // Queue one completion and take the real F5 that transfers it into the
    // cartridge's block buffer. Without this the buffer holds nothing to find.
    gpk_report_t warm;
    memset(&warm, 0, sizeof(warm));
    (void)gpk_stage_block_roundtrip(&warm);

    u32 n = 0;
    for (u32 li = 0; li < 4; li++) {
        for (u32 pi = 0; pi < 2; pi++) {
            if (n >= GPK_F5_VARIANTS)
                break;
            gpk_f5_variant_t *v = &out[n];
            v->latency = kLatencies[li];
            v->prime   = pi;
            v->name    = pi ? "prime" : "plain";

            gpkLatencyBlockRead = v->latency;
            gpkPrimeBlockRead   = pi;

            memset(sReadBlock, 0, sizeof(sReadBlock));
            gpk_read_block(sReadBlock, 0);

            v->head0 = *(const u32 *)sReadBlock;
            v->magic_offset = GPK_F5_NO_MAGIC;
            // Scan on 4-byte boundaries: the skew is whole words, and an
            // unaligned hit would mean something quite different is wrong.
            for (u32 off = 0; off + 4 <= GPK_BLOCK_BYTES; off += 4) {
                u32 w;
                memcpy(&w, sReadBlock + off, sizeof(w));
                if (w == GPK_COMP_MAGIC) {
                    v->magic_offset = off;
                    break;
                }
            }
            n++;
        }
    }

    gpkLatencyBlockRead = savedLatency;
    gpkPrimeBlockRead   = savedPrime;
    return n;
}

u32 gpk_f4_matrix(gpk_f4_variant_t *out)
{
    // Refuse to run on a dead link. The first matrix attempt ran after a USB
    // mass-storage session had left the cartridge wedged - B8 read 00000000 and
    // HELLO did not answer - so every variant "failed" for a reason that had
    // nothing to do with F4. Numbers gathered over a broken bus are worse than
    // no numbers, because they look like evidence.
    u32 protocol = 0;
    if (gpk_hello(&protocol, NULL, NULL, NULL) != GPK_OK || protocol != GPK_PROTOCOL_V1)
        return 0;

    u32 handle = 0, size = 0;
    handle = gpk_alloc(sizeof(gpkTestPattern), &size);
    if (handle == 0)
        return 0;
    (void)gpk_upload(handle, gpkTestPattern, sizeof(gpkTestPattern));

    static const char *kNames[GPK_F4_VARIANTS] = {
        "80B inline", "512B inline", "64B noinl",
        "primed", "lat32", "lat63",
    };

    for (u32 v = 0; v < GPK_F4_VARIANTS; v++) {
        gpk_drain_completions();
        u32 e0 = gpk_read_reg(0xF0), a0 = gpk_read_reg(0xF1);
        u32 c0 = gpk_read_reg(0xF2), p0 = gpk_read_reg(0xF3);
        u32 savedLatency = gpkLatencyWrite;

        memset(sBlock, 0, sizeof(sBlock));
        u32 meaningful = GPK_DESCRIPTOR_BYTES + sizeof(gpkTestPattern);
        u32 flags = GPK_FLAG_INLINE_INPUT;

        switch (v) {
        case 1: meaningful = GPK_BLOCK_BYTES; break;      // declare the whole block
        case 2: meaningful = GPK_DESCRIPTOR_BYTES;        // no inline payload at all
                flags = 0; break;
        case 4: gpkLatencyWrite = 32; break;              // more write latency
        case 5: gpkLatencyWrite = 63; break;              // maximum write latency
        default: break;
        }

        gpk_build_descriptor((gpk_descriptor_t *)sBlock, 200 + v, handle,
                             sizeof(gpkTestPattern), flags, 100000);
        if (flags & GPK_FLAG_INLINE_INPUT)
            memcpy(sBlock + GPK_DESCRIPTOR_BYTES, gpkTestPattern, sizeof(gpkTestPattern));

        if (v == 3) {
            memset(sPrimeBlock, 0, sizeof(sPrimeBlock));
            gpk_write_block(sPrimeBlock, GPK_BLOCK_BYTES);
        }

        gpk_write_block(sBlock, meaningful);

        u32 depth = 0;
        for (int i = 0; i < 8 && depth == 0; i++) {
            depth = gpk_event_depth();
            if (depth == 0)
                swiDelay(500);
        }

        // Report absolute counters, not deltas. An undriven read makes an
        // unsigned delta wrap to ~4e9 and masquerade as a huge count, which is
        // how the first matrix run produced "a4294967294" and told us nothing.
        (void)e0; (void)a0; (void)c0; (void)p0;
        gpk_tick();
        out[v].name        = kNames[v];
        out[v].completions = depth;
        out[v].enter       = gpk_read_reg(0xF0);
        out[v].accepted    = gpk_read_reg(0xF1);
        out[v].complete    = gpk_read_reg(0xF2);
        out[v].parsed      = gpk_read_reg(0xF3);
        gpkLatencyWrite    = savedLatency;
    }

    gpk_write_reg(GPK_REG_ARG0, handle);
    gpk_exec(GPK_CMD_FREE);
    return GPK_F4_VARIANTS;
}

bool gpk_run_quick(gpk_report_t *r)
{
    memset(r, 0, sizeof(*r));
    if (!gpk_stage_discovery(r))
        return false;
    gpk_stage_legacy(r);
    bool block_ok = gpk_stage_block_roundtrip(r);

    // Read the RP2040 F4 counters here, not inside the stage above.
    //
    // They were previously read after the block write, which is past an early
    // return taken when the allocation fails - so a run that never got as far
    // as sending an F4 reported e0 a0 c0 p0, which reads exactly like "the
    // command never arrived". Those are different faults and must not look the
    // same. Reading unconditionally here means the counters always describe
    // what actually happened on the cartridge.
    r->f4_enter    = gpk_read_reg(0xF0);
    r->f4_accepted = gpk_read_reg(0xF1);
    r->f4_complete = gpk_read_reg(0xF2);
    r->f4_parsed   = gpk_read_reg(0xF3);

    r->overall_ok = r->device_present && r->legacy_ok && block_ok;
    return r->overall_ok;
}

bool gpk_run_full(gpk_report_t *r)
{
    if (!gpk_run_quick(r))
        return false;
    gpk_bench_cmd_latency(&r->cmd_latency);
    gpk_bench_f4(&r->f4_latency, GPK_BLOCK_BYTES);
    gpk_bench_f5(&r->f5_latency);
    gpk_bench_rtt(&r->rtt_512);
    gpk_compute_bandwidth(r);
    gpk_bench_sizes(r);
    bool batch_ok = gpk_bench_batch(r);
    r->overall_ok = r->overall_ok && batch_ok;
    return r->overall_ok;
}
