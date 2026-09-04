// gekkopak_test.nds - GekkoPAK v1 physical cartridge transport validator.
//
// This is deliberately NOT a GameCube runtime. It exercises the real NTR
// cartridge bus against a DSpico running the GekkoPAK F0-F5 overlay and reports
// measured timings, so the simulator's provisional 6 MiB/s / 25 us numbers can
// be replaced with facts.

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"

static PrintConsole sTop;
static PrintConsole sBottom;
static gpk_report_t sReport;
static bool sHaveReport;
static bool sFullRun;
static bool sFatReady;

#define LOG(...) do { consoleSelect(&sBottom); iprintf(__VA_ARGS__); consoleSelect(&sTop); } while (0)

static const char *pf(bool ok) { return ok ? "PASS" : "FAIL"; }

static const char *layer_name(int layer)
{
    switch (layer) {
    case GPK_LAYER_NONE:       return "ok";
    case GPK_LAYER_CARD_OWNER: return "slot-1 owner (EXMEMCNT)";
    case GPK_LAYER_UNSCRAMBLE: return "cartridge not in unscrambled game mode";
    case GPK_LAYER_HELLO:      return "F1/F2 HELLO no answer";
    case GPK_LAYER_PROTOCOL:   return "protocol mismatch";
    default:                   return "unknown";
    }
}

// KiB/s -> "X.XX MiB/s" without floating point in the format string.
static void fmt_mib(u32 kib_per_s, char *out, size_t n)
{
    u32 hundredths = (u32)(((u64)kib_per_s * 100ull) / 1024ull);
    siprintf(out, "%lu.%02lu MiB/s",
             (unsigned long)(hundredths / 100), (unsigned long)(hundredths % 100));
    (void)n;
}

static void draw_status(void)
{
    consoleSelect(&sTop);
    consoleClear();
    iprintf("GEKKOPAK PHYSICAL TEST\n\n");

    if (!sHaveReport) {
        iprintf("Press A  quick test\n");
        iprintf("Press X  full benchmark\n");
        return;
    }

    const gpk_report_t *r = &sReport;
    iprintf("DSpico RP2040\n");
    if (!r->device_present) {
        iprintf("Device detected  NO\n\n");
        iprintf("Failing layer:\n  %s\n\n", layer_name(r->init_layer));
        iprintf("Overall         FAIL\n");
        return;
    }

    iprintf("Device detected  YES\n");
    iprintf("Protocol        %lu.%lu\n",
            (unsigned long)(r->protocol >> 16), (unsigned long)(r->protocol & 0xFFFF));
    iprintf("Caps            %08lX\n", (unsigned long)r->caps);
    iprintf("Local RAM       %lu KiB\n", (unsigned long)(r->local_bytes / 1024));
    iprintf("F4/F5 support   %s\n", (r->caps & GPK_CAP_BLOCK_XPORT) ? "YES" : "NO");
    iprintf("F4/F5           %s\n", pf(r->f4_ok && r->f5_ok));
    iprintf("Checksum        %s\n", pf(r->checksum_ok));

    if (!sFullRun) {
        iprintf("\nchecksum  %08lX\n", (unsigned long)r->checksum);
        iprintf("\nOverall         %s\n", pf(r->overall_ok));
        return;
    }

    char buf[32];
    fmt_mib(r->write_kib_per_s, buf, sizeof(buf));
    iprintf("\nWrite BW        %s\n", buf);
    fmt_mib(r->read_kib_per_s, buf, sizeof(buf));
    iprintf("Read BW         %s\n", buf);
    iprintf("\nCmd latency     %lu us\n", (unsigned long)r->cmd_latency.median_us);
    iprintf("512B RTT        %lu us\n\n", (unsigned long)r->rtt_512.median_us);

    for (u32 i = 0; i < r->batch_count; i++) {
        const gpk_batch_result_t *b = &r->batches[i];
        iprintf("Batch %lu         %lu.%03lu us/job%s\n",
                (unsigned long)b->batch,
                (unsigned long)(b->us_per_job_x1000 / 1000),
                (unsigned long)(b->us_per_job_x1000 % 1000),
                b->ok ? "" : " !");
    }
    iprintf("\nOverall         %s\n", pf(r->overall_ok));
}

static void log_details(void)
{
    const gpk_report_t *r = &sReport;
    LOG("--- detail ---\n");
    LOG("init layer : %s\n", layer_name(r->init_layer));
    LOG("latency r/w: %lu / %lu cycles\n",
        (unsigned long)r->latency_read, (unsigned long)r->latency_write);
    LOG("legacy F0-F3 %s (%08lX)\n", pf(r->legacy_ok), (unsigned long)r->legacy_checksum);
    LOG("event depth: %lu\n", (unsigned long)r->event_depth);
    LOG("checksum   : %08lX exp %08lX\n",
        (unsigned long)r->checksum, (unsigned long)GPK_EXPECTED_CHECKSUM);
    if (!sFullRun)
        return;
    LOG("cmd  min/med/p95/max %lu/%lu/%lu/%lu\n",
        (unsigned long)r->cmd_latency.min_us, (unsigned long)r->cmd_latency.median_us,
        (unsigned long)r->cmd_latency.p95_us, (unsigned long)r->cmd_latency.max_us);
    LOG("F4   min/med/p95/max %lu/%lu/%lu/%lu\n",
        (unsigned long)r->f4_latency.min_us, (unsigned long)r->f4_latency.median_us,
        (unsigned long)r->f4_latency.p95_us, (unsigned long)r->f4_latency.max_us);
    LOG("F5   min/med/p95/max %lu/%lu/%lu/%lu\n",
        (unsigned long)r->f5_latency.min_us, (unsigned long)r->f5_latency.median_us,
        (unsigned long)r->f5_latency.p95_us, (unsigned long)r->f5_latency.max_us);
    for (u32 i = 0; i < r->size_count; i++) {
        LOG("%4lu B word %lu us block %lu us\n",
            (unsigned long)r->sizes[i].bytes,
            (unsigned long)r->sizes[i].word_path.median_us,
            (unsigned long)r->sizes[i].block_path.median_us);
    }
}

// ---------------------------------------------------------------------------
// Result export. Writes both a human report and a machine-readable CSV so the
// simulator can ingest the numbers directly.
// ---------------------------------------------------------------------------
static void write_report_files(void)
{
    if (!sFatReady) {
        LOG("SD not available; skipping save\n");
        return;
    }
    mkdir("/gekkopak", 0777);
    mkdir("/gekkopak/results", 0777);

    const gpk_report_t *r = &sReport;
    FILE *f = fopen("/gekkopak/results/latest.txt", "w");
    if (f) {
        fprintf(f, "GekkoPAK DSpico v1 hardware result\n");
        fprintf(f, "build       %s %s\n", __DATE__, __TIME__);
        fprintf(f, "protocol    %lu.%lu\n",
                (unsigned long)(r->protocol >> 16), (unsigned long)(r->protocol & 0xFFFF));
        fprintf(f, "caps        %08lX\n", (unsigned long)r->caps);
        fprintf(f, "local_bytes %lu\n", (unsigned long)r->local_bytes);
        fprintf(f, "latency_r/w %lu/%lu\n",
                (unsigned long)r->latency_read, (unsigned long)r->latency_write);
        fprintf(f, "legacy      %s\n", pf(r->legacy_ok));
        fprintf(f, "f4/f5       %s/%s\n", pf(r->f4_ok), pf(r->f5_ok));
        fprintf(f, "checksum    %08lX (%s)\n", (unsigned long)r->checksum, pf(r->checksum_ok));
        fprintf(f, "cmd_us      %lu\n", (unsigned long)r->cmd_latency.median_us);
        fprintf(f, "write_kibps %lu\n", (unsigned long)r->write_kib_per_s);
        fprintf(f, "read_kibps  %lu\n", (unsigned long)r->read_kib_per_s);
        fprintf(f, "rtt512_us   %lu\n", (unsigned long)r->rtt_512.median_us);
        for (u32 i = 0; i < r->batch_count; i++)
            fprintf(f, "batch%lu_us   %lu.%03lu\n",
                    (unsigned long)r->batches[i].batch,
                    (unsigned long)(r->batches[i].us_per_job_x1000 / 1000),
                    (unsigned long)(r->batches[i].us_per_job_x1000 % 1000));
        fprintf(f, "overall     %s\n", pf(r->overall_ok));
        fclose(f);
    }

    f = fopen("/gekkopak/results/latest.csv", "w");
    if (f) {
        fprintf(f, "metric,unit,min,median,mean,p95,max\n");
        const struct { const char *n; const gpk_stats_t *s; } rows[] = {
            { "cmd_latency", &r->cmd_latency },
            { "f4_write_512", &r->f4_latency },
            { "f5_read_512", &r->f5_latency },
            { "rtt_512", &r->rtt_512 },
        };
        for (u32 i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
            fprintf(f, "%s,us,%lu,%lu,%lu,%lu,%lu\n", rows[i].n,
                    (unsigned long)rows[i].s->min_us, (unsigned long)rows[i].s->median_us,
                    (unsigned long)rows[i].s->mean_us, (unsigned long)rows[i].s->p95_us,
                    (unsigned long)rows[i].s->max_us);
        for (u32 i = 0; i < r->size_count; i++)
            fprintf(f, "size_%lu_word,us,,%lu,,,\nsize_%lu_block,us,,%lu,,,\n",
                    (unsigned long)r->sizes[i].bytes,
                    (unsigned long)r->sizes[i].word_path.median_us,
                    (unsigned long)r->sizes[i].bytes,
                    (unsigned long)r->sizes[i].block_path.median_us);
        for (u32 i = 0; i < r->batch_count; i++)
            fprintf(f, "batch_%lu,us_per_job,,%lu.%03lu,,,\n",
                    (unsigned long)r->batches[i].batch,
                    (unsigned long)(r->batches[i].us_per_job_x1000 / 1000),
                    (unsigned long)(r->batches[i].us_per_job_x1000 % 1000));
        fclose(f);
    }
    LOG("saved /gekkopak/results/latest.{txt,csv}\n");
}

static void run(bool full)
{
    sFullRun = full;
    LOG("running %s...\n", full ? "full benchmark" : "quick test");
    if (full)
        gpk_run_full(&sReport);
    else
        gpk_run_quick(&sReport);
    sHaveReport = true;
    draw_status();
    log_details();
    LOG("done: %s\n", pf(sReport.overall_ok));
}

// Early boot progress marker.
//
// A blank screen tells you nothing about where a DS app died, so before any
// libnds subsystem is brought up we paint the main screen directly from a
// bitmap-mode framebuffer. Each stage advances the colour, so a hang is located
// by eye without a debugger:
//
//   white  - ARM9 never reached main(); the ROM or boot path is at fault
//   red    - reached main()
//   yellow - video/VRAM configured
//   green  - both text consoles up
//   blue   - reached the main loop (normal running state)
//
// This is the "smallest isolated reproducer" rule applied to bring-up: it costs
// a few lines and turns "white screen" into a specific failing stage.
static void dbg_stage(u16 colour)
{
    videoSetMode(MODE_FB0);
    vramSetBankA(VRAM_A_LCD);
    for (int i = 0; i < 256 * 192; i++)
        VRAM_A[i] = colour | BIT(15);
}

int main(void)
{
    powerOn(POWER_ALL_2D);
    dbg_stage(RGB15(31, 0, 0));   // red: main() entered

    // The marker owns VRAM_A as an LCD framebuffer, so it must be the last
    // thing to touch the main engine before the background setup below. An
    // earlier version painted a second marker *after* vramSetBankA(), which
    // silently put VRAM_A back into LCD mode and left the top screen stuck on
    // the marker colour while the sub-screen console worked fine.
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    consoleInit(&sTop, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&sBottom, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
    // consoleInit reclaims VRAM_A for backgrounds, so the marker stops here and
    // any further progress is reported as text.

    consoleSelect(&sBottom);
    iprintf("GekkoPAK DSpico v1 log\n");
    iprintf("A=quick X=full START=rerun\n");
    iprintf("SELECT=save report\n\n");

    // fatInitDefault() is NOT called at startup. This ROM is not dlditool
    // patched with the DSpico DLDI driver, so libfat has no valid driver to
    // probe and the call is a plausible early hang. SD export is optional to
    // the benchmark, so it is deferred behind SELECT where a failure costs
    // nothing but a message.
    sFatReady = false;
    iprintf("SD: deferred (press SELECT)\n");

    // Bus probe, before any GekkoPAK traffic.
    //
    // Each line isolates one layer, so a failure points at a specific thing
    // rather than at "the hardware":
    //   B8 card id  - stock game-mode command. A sane value means the ROMCTRL
    //                 setup, slot-1 ownership and command serialization work.
    //   E4 sd stat  - a DSpico extension valid only in unscrambled game mode.
    //                 Answering proves we are in that mode and that DSpico's
    //                 extended dispatch reaches us.
    //   F2 arg0     - GekkoPAK. If B8 and E4 answer but this does not, the
    //                 fault is in the overlay's F0-F5 handlers, not the bus.
    REG_EXMEMCNT &= ~ARM7_OWNS_CARD;
    LOG("--- bus probe ---\n");
    LOG("EXMEMCNT   : %04X (arm9 owns card: %s)\n",
        (unsigned)REG_EXMEMCNT, (REG_EXMEMCNT & ARM7_OWNS_CARD) ? "NO" : "yes");
    LOG("B8 card id : %08lX\n", (unsigned long)gpk_raw_read32(0xB800000000000000ull));
    LOG("E4 sd stat : %08lX\n", (unsigned long)gpk_raw_read32(0xE400000000000000ull));

    // Read-latency sweep.
    //
    // B8 and E4 answer reliably while F2 mostly returns FFFFFFFF, and HELLO
    // succeeds only occasionally. Those stock handlers are trivial, whereas
    // GekkoPAK's F2 runs commandMatches() and bounds checks before
    // ntrc_beginWrite(), so the likely cause is that LATENCY2(4) - the
    // documented minimum - does not leave the RP2040 enough time to have the
    // word queued. Rather than guess a bigger number, find the threshold: it
    // is a real property of this cartridge and it bounds the achievable
    // transaction rate, so the benchmark needs it either way.
    static const u32 kLatencies[] = { 4, 8, 12, 16, 24, 32, 48, 63 };
    LOG("F2 read latency sweep (of 32):\n");
    u32 chosen = 0;
    for (u32 i = 0; i < sizeof(kLatencies) / sizeof(kLatencies[0]); i++) {
        gpkLatencyRead = kLatencies[i];
        gpk_write_reg(GPK_REG_ARG0, 0xA5A5A5A5u);
        u32 ok = 0;
        for (u32 n = 0; n < 32; n++) {
            if (gpk_read_reg(GPK_REG_ARG0) == 0xA5A5A5A5u)
                ok++;
        }
        LOG("  lat %2lu : %2lu\n", (unsigned long)kLatencies[i], (unsigned long)ok);
        if (ok == 32 && chosen == 0)
            chosen = kLatencies[i];
    }
    // Take margin over the first fully reliable value; if none worked, keep the
    // largest so the benchmark still reports something rather than silently
    // measuring failed transactions.
    gpkLatencyRead = chosen ? chosen : 63;
    gpkLatencyWrite = gpkLatencyRead * 2 > 63 ? 63 : gpkLatencyRead * 2;
    LOG("using lat r/w %lu/%lu\n",
        (unsigned long)gpkLatencyRead, (unsigned long)gpkLatencyWrite);

    consoleSelect(&sTop);
    draw_status();

    bool marked = false;
    while (pmMainLoop()) {
        swiWaitForVBlank();
        if (!marked) {
            // Reached the main loop: report it on the log screen, since VRAM_A
            // now belongs to the console rather than the bitmap marker.
            LOG("boot: reached main loop\n");
            marked = true;
        }
        scanKeys();
        u32 keys = keysDown();
        if (keys & KEY_A)
            run(false);
        else if (keys & KEY_X)
            run(true);
        else if (keys & KEY_START)
            run(sFullRun);
        else if (keys & KEY_SELECT) {
            if (!sFatReady) {
                LOG("init SD (libfat)...\n");
                sFatReady = fatInitDefault();
                LOG("SD: %s\n", sFatReady ? "ready" : "unavailable");
            }
            if (sHaveReport)
                write_report_files();
            else
                LOG("no report yet\n");
        }
    }
    return 0;
}
