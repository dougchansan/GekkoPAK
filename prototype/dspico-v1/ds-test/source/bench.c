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
    // Read the RP2040-side F4 counters. These separate three cases the host
    // cannot otherwise tell apart: the command never arriving, arriving but
    // being rejected by the opcode/index/length checks, arriving and being
    // accepted but the payload never completing, and the payload completing but
    // the descriptor being rejected.
    r->f4_enter    = gpk_read_reg(0xF0);
    r->f4_accepted = gpk_read_reg(0xF1);
    r->f4_complete = gpk_read_reg(0xF2);
    r->f4_parsed   = gpk_read_reg(0xF3);

    if (r->event_depth == 0) {
        r->f5_ok = false;
        return false;
    }

    memset(sReadBlock, 0, sizeof(sReadBlock));
    gpk_read_block(sReadBlock, GPK_COMPLETION_BYTES);
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

bool gpk_run_quick(gpk_report_t *r)
{
    memset(r, 0, sizeof(*r));
    if (!gpk_stage_discovery(r))
        return false;
    gpk_stage_legacy(r);
    bool block_ok = gpk_stage_block_roundtrip(r);
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
