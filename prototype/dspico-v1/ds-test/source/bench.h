#ifndef GEKKOPAK_BENCH_H
#define GEKKOPAK_BENCH_H

#include "gekkopak_ntr.h"

#define GPK_BENCH_ITERATIONS 100
#define GPK_BENCH_WARMUP     16

// The deterministic 16-byte GekkoPAK test payload. Its FNV-1a is 0xf269b734
// when the bytes reach DSpico SRAM in this order, which is the assertion that
// validates the whole write path end to end.
#define GPK_EXPECTED_CHECKSUM 0xf269b734u
extern const u8 gpkTestPattern[16];

typedef struct {
    u32 min_us;
    u32 median_us;
    u32 mean_us;
    u32 p95_us;
    u32 max_us;
    u32 samples;
} gpk_stats_t;

typedef struct {
    u32 bytes;
    gpk_stats_t word_path;  // F3, 4 bytes per transaction
    gpk_stats_t block_path; // F4, one 512-byte bus transfer
} gpk_size_result_t;

typedef struct {
    u32 batch;
    u32 us_per_job_x1000;
    u32 transactions_x1000;   // transactions per job, x1000
    u32 completions;
    bool ok;
} gpk_batch_result_t;

typedef struct {
    bool  device_present;
    int   init_layer;
    u32   protocol;
    u32   caps;
    u32   local_bytes;
    u32   transport;
    bool  f4_ok;
    bool  f5_ok;
    bool  checksum_ok;
    u32   checksum;
    u32   legacy_checksum;
    bool  legacy_ok;
    u32   event_depth;
    // RP2040-side F4 counters, read back over F2 indices 0xF0-0xF3.
    u32   f4_enter, f4_accepted, f4_complete, f4_parsed;
    // Allocation handle obtained by the block stage; 0 means it never got that far.
    u32   block_handle;
    gpk_stats_t cmd_latency;
    gpk_stats_t f4_latency;
    gpk_stats_t f5_latency;
    gpk_stats_t rtt_512;
    u32   write_kib_per_s;
    u32   read_kib_per_s;
    gpk_size_result_t sizes[7];
    u32   size_count;
    gpk_batch_result_t batches[4];
    u32   batch_count;
    bool  overall_ok;
    u32   latency_read;
    u32   latency_write;
} gpk_report_t;

void gpk_stats_compute(u32 *samples, u32 count, gpk_stats_t *out);
bool gpk_run_quick(gpk_report_t *r);
bool gpk_run_full(gpk_report_t *r);

#endif
