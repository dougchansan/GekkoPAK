// gekkopak_test.nds - GekkoPAK v1 physical cartridge transport validator.
//
// This is deliberately NOT a GameCube runtime. It exercises the real NTR
// cartridge bus against a DSpico running the GekkoPAK F0-F5 overlay and reports
// measured timings, so the simulator's provisional 6 MiB/s / 25 us numbers can
// be replaced with facts.

#include <nds.h>
#include <fat.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bench.h"

static PrintConsole sTop;
static PrintConsole sBottom;
static gpk_report_t sReport;
static bool sHaveReport;
static bool sFullRun;
static bool sFatReady;

// Every log line is mirrored into sDiag so the full diagnostic transcript can
// be written to the SD card. Reading numbers off a photograph of a DS screen is
// slow and error-prone, and the transcript is the primary artefact of a run.
static char sDiag[8192];
static u32  sDiagLen;

static void gpk_log(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    vsniprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    consoleSelect(&sBottom);
    iprintf("%s", line);
    consoleSelect(&sTop);

    u32 n = strlen(line);
    if (sDiagLen + n + 1 < sizeof(sDiag)) {
        memcpy(sDiag + sDiagLen, line, n);
        sDiagLen += n;
        sDiag[sDiagLen] = 0;
    }
}

#define LOG(...) gpk_log(__VA_ARGS__)

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
// Result export. Writes the diagnostic transcript, a human report and a
// machine-readable CSV so the simulator can ingest the numbers directly.
// ---------------------------------------------------------------------------

// libfat mounts as "fat:/" on DS and "sd:/" on DSi, and a bare "/" only works
// when the default device happens to be set. Rather than assume, find a prefix
// a file can actually be created and read back through. Empty until verified.
static char sPrefix[8];

static bool gpk_join(char *out, size_t n, const char *tail)
{
    if (!sPrefix[0])
        return false;
    siprintf(out, "%s%s", sPrefix, tail);
    (void)n;
    return true;
}

static void gpk_find_prefix(void)
{
    static const char *kPrefixes[] = { "fat:/", "sd:/", "/" };
    for (u32 i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); i++) {
        char dir[64], path[72], back[32];
        siprintf(dir, "%sgekkopak", kPrefixes[i]);
        mkdir(dir, 0777);
        siprintf(dir, "%sgekkopak/results", kPrefixes[i]);
        mkdir(dir, 0777);
        siprintf(path, "%sgekkopak/results/probe.txt", kPrefixes[i]);

        FILE *f = fopen(path, "w");
        if (!f)
            continue;
        fputs("gekkopak-write-probe\n", f);
        fclose(f);

        // Read it back. A successful fopen("w") is not proof the bytes reached
        // the card - that assumption is exactly what made an earlier run report
        // "saved" while nothing landed.
        back[0] = 0;
        f = fopen(path, "r");
        if (f) {
            if (!fgets(back, sizeof(back), f))
                back[0] = 0;
            fclose(f);
        }
        if (strncmp(back, "gekkopak-write-probe", 20) == 0) {
            siprintf(sPrefix, "%s", kPrefixes[i]);
            LOG("SD write ok via \"%s\"\n", sPrefix);
            return;
        }
    }
    LOG("SD write: no working prefix\n");
}

// Write just the log transcript. Used at startup, where the probe output is
// already the useful artefact and no benchmark has run yet.
static void write_diag_only(void)
{
    char path[72];
    if (!sFatReady || !sPrefix[0]) {
        LOG("SD not writable; no diag\n");
        return;
    }
    gpk_join(path, sizeof(path), "gekkopak/results/diag.txt");
    FILE *d = fopen(path, "w");
    if (d) {
        fwrite(sDiag, 1, sDiagLen, d);
        fclose(d);
    }
    LOG("diag %s\n", d ? "saved" : "FAILED");
}

static void write_report_files(void)
{
    char path[64];
    if (!sFatReady || !sPrefix[0]) {
        LOG("SD not writable; skipping save\n");
        return;
    }

    // Diagnostic transcript first: it is the primary artefact of a run, and
    // when a run fails it is the transcript that says why.
    gpk_join(path, sizeof(path), "gekkopak/results/diag.txt");
    FILE *d = fopen(path, "w");
    if (d) {
        fwrite(sDiag, 1, sDiagLen, d);
        fclose(d);
    }

    const gpk_report_t *r = &sReport;
    gpk_join(path, sizeof(path), "gekkopak/results/latest.txt");
    FILE *f = fopen(path, "w");
    bool wrote_txt = (f != NULL);
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

    gpk_join(path, sizeof(path), "gekkopak/results/latest.csv");
    f = fopen(path, "w");
    bool wrote_csv = (f != NULL);
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
    // No unmount/remount. probe.txt persisted through a plain fclose, so the
    // writeback happens without one - and the remount was failing, leaving
    // sFatReady false. That is why the last run logged "diag saved" and then
    // "SD not writable; skipping save" for the results that followed.
    LOG("save: diag %s txt %s csv %s\n",
        d ? "ok" : "FAIL", wrote_txt ? "ok" : "FAIL", wrote_csv ? "ok" : "FAIL");
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
    // Autosave. The transcript and CSV are the point of the exercise, and
    // depending on someone remembering a keypress loses runs.
    write_report_files();
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

    // SD init runs at startup again. It was deferred when an unpatched ROM made
    // libfat a plausible boot hang, but the cartridge now boots through the
    // DSpico Bootloader and Pico Loader, which supply a real DLDI driver.
    sFatReady = fatInitDefault();
    iprintf("SD: %s\n", sFatReady ? "mounted" : "unavailable");
    if (sFatReady)
        gpk_find_prefix();

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

    // EXEC settle sweep.
    //
    // The register sweep above passes at every latency, so the register path is
    // sound. What fails is HELLO, which is the only thing that issues F1 EXEC -
    // and DSpico runs executeHighCommand() inside the card IRQ, so a following
    // F2 arrives while the RP2040 is still busy. Measure how much settle time
    // it actually needs instead of picking a number; the answer bounds the cost
    // of every EXEC-based operation and belongs in the results.
    static const u32 kSettles[] = { 0, 16, 64, 256, 1024 };
    LOG("EXEC settle sweep (HELLO, of 16):\n");
    u32 settle = 0;
    for (u32 i = 0; i < sizeof(kSettles) / sizeof(kSettles[0]); i++) {
        gpkExecSettle = kSettles[i];
        u32 ok = 0;
        for (u32 n = 0; n < 16; n++) {
            u32 proto = 0;
            if (gpk_hello(&proto, NULL, NULL, NULL) == GPK_OK && proto == GPK_PROTOCOL_V1)
                ok++;
        }
        LOG("  settle %4lu : %2lu\n", (unsigned long)kSettles[i], (unsigned long)ok);
        if (ok == 16 && settle == 0 && kSettles[i] != 0)
            settle = kSettles[i];
    }
    // HELLO is the cheapest EXEC there is - executeHighCommand() just assigns
    // four values. ALLOC scans the allocation table and UPLOAD does a copy plus
    // FNV-1a, so the minimum that satisfies HELLO is too tight for them. Take a
    // wide margin; EXEC is not on the timed benchmark path, so this costs
    // nothing that matters.
    settle = settle ? settle * 8 : 1024;
    gpkExecSettle = settle > 4096 ? 4096 : settle;
    LOG("using settle %lu (8x margin)\n", (unsigned long)gpkExecSettle);

    // Step-by-step legacy probe. `legacy F0-F3 FAIL (00000000)` does not say
    // which step broke or why, and the GekkoPAK result codes are specific
    // (2=BadHandle 3=NoMemory 4=NotReady 5=BadDescriptor 7=QueueFull), so print
    // them rather than infer.
    LOG("--- legacy probe ---\n");
    gpk_write_reg(GPK_REG_ARG0, 0xDEADBEEFu);
    LOG("reg rt     : %08lX\n", (unsigned long)gpk_read_reg(GPK_REG_ARG0));
    gpk_write_reg(GPK_REG_ARG0, 16);
    gpk_exec(GPK_CMD_ALLOC);

    // Read into locals first. Passing several gpk_read_reg() calls as arguments
    // to one printf leaves their order unspecified, and each one is a bus
    // transaction - the previous run's confusing "RESULT=255, OUT0=FFFFFFFF,
    // size=16" was read in an unknown sequence, so it could not be interpreted.
    u32 pres = gpk_read_reg(GPK_REG_RESULT);
    u32 phandle = gpk_read_reg(GPK_REG_OUT0);
    u32 psize = gpk_read_reg(GPK_REG_OUT1);
    LOG("alloc res %08lX h %08lX sz %lu\n",
        (unsigned long)pres, (unsigned long)phandle, (unsigned long)psize);

    // Same register four times in a row: if a value settles after the first
    // read, this is a wake or turnaround effect; if it is stable but wrong,
    // it is not timing at all.
    LOG("res x4 :");
    for (u32 n = 0; n < 4; n++)
        LOG(" %08lX", (unsigned long)gpk_read_reg(GPK_REG_RESULT));
    LOG("\n");
    LOG("out0x4 :");
    for (u32 n = 0; n < 4; n++)
        LOG(" %08lX", (unsigned long)gpk_read_reg(GPK_REG_OUT0));
    LOG("\n");

    // Save the startup transcript before waiting for input. The bus probe,
    // latency sweep, settle sweep and legacy probe all run unattended and are
    // the diagnostic data that matters right now, so they must not depend on
    // anyone pressing a key afterwards.
    write_diag_only();

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
            // Results are saved automatically after every run; SELECT just
            // forces another write.
            if (sHaveReport)
                write_report_files();
            else
                LOG("no report yet\n");
        }
    }
    return 0;
}
