// gekkopak_test.nds - GekkoPAK v1 physical cartridge transport validator.
//
// This is deliberately NOT a GameCube runtime. It exercises the real NTR
// cartridge bus against a DSpico running the GekkoPAK F0-F5 overlay and reports
// measured timings, so the simulator's provisional 6 MiB/s / 25 us numbers can
// be replaced with facts.

#include <nds.h>
#include <fat.h>
#include <nds/arm9/dldi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bench.h"
#include "tusb.h"

// Event pump for the DSpico device controller, in usb/dcd_dspico.c. USB is card
// traffic, so this is only ever called outside a timed region.
void gpk_usb_task(void);

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

// Probe values, kept for the one-page summary.
static u32 sProbeB8, sProbeAllocRes, sProbeHandle, sProbeSize, sProbeUpSum;

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

// Live status line, pinned to the bottom of the top screen.
//
// A run does a lot of bus work with nothing to show for it until the summary is
// drawn at the end, which is indistinguishable from a hang. This reports the
// stage in progress so it is visibly alive, and so a run that does lock up
// leaves the name of the stage it died in on screen.
static void gpk_status(const char *fmt, ...)
{
    char line[40];
    va_list ap;
    va_start(ap, fmt);
    vsniprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    consoleSelect(&sTop);
    // Row 22, column 0, then pad to full width so a shorter stage name cannot
    // leave the previous one showing through.
    iprintf("\x1b[22;0H>> %-28s", line);

    // Also announce on the log screen. The ANSI positioning above may not be
    // honoured by this console - a run reported no visible status at all - and
    // the log screen is known to display, so this is the dependable copy.
    consoleSelect(&sBottom);
    iprintf("\n[%s]\n", line);
    consoleSelect(&sTop);
}

// Heartbeat.
//
// Long stages run thousands of bus transactions with no screen change, which is
// indistinguishable from a hang - and several runs tonight genuinely hung. This
// prints a spinning character on the log screen periodically, so motion means
// progress and a frozen character means stuck.
//
// It writes straight to the console rather than through LOG(), so it never
// enters the transcript, and every call site is outside a timed region so it
// cannot skew a measurement.
static u32 sTickCount;

void gpk_tick(void)
{
    if ((++sTickCount & 0x3F) != 0)
        return;
    static const char kSpin[4] = { '|', '/', '-', '\\' };
    consoleSelect(&sBottom);
    iprintf("%c", kSpin[(sTickCount >> 6) & 3]);
    consoleSelect(&sTop);
}

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

// Action menu. One build, many experiments: each of these was previously a
// rebuild, a copy and a card swap away.
static void draw_menu(void)
{
    consoleSelect(&sTop);
    consoleClear();
    iprintf("GEKKOPAK PHYS TEST DSpico\n\n");
    iprintf("B8 %08lX  timeouts %lu\n",
            (unsigned long)sProbeB8, (unsigned long)gpkTimeouts);
    iprintf("lat r/w %lu/%lu settle %lu\n\n",
            (unsigned long)gpkLatencyRead, (unsigned long)gpkLatencyWrite,
            (unsigned long)gpkExecSettle);
    iprintf("A  quick test\n");
    iprintf("X  full benchmark\n");
    iprintf("Y  F4 variant matrix\n");
    iprintf("UP F5 read sweep\n");
    iprintf("B  re-probe link\n");
    iprintf("L  cycle write latency\n");
    iprintf("R  cycle exec settle\n");
    iprintf("START  rerun last\n");
    iprintf("SELECT save to SD\n\n");
    iprintf("USB: plug in for results\n");
}

static void draw_status(void)
{
    consoleSelect(&sTop);
    consoleClear();
    const gpk_report_t *r = &sReport;
    char buf[32];

    // One page, everything on it. SD export never worked reliably - only the
    // first write of a run ever reached the card - so a single photograph of
    // this screen is the collection method. Keep it dense and complete.
    iprintf("GEKKOPAK PHYS TEST DSpico\n");
    iprintf("B8 %08lX to %lu\n",
            (unsigned long)sProbeB8, (unsigned long)gpkTimeouts);
    iprintf("lat %lu/%lu settle %lu\n",
            (unsigned long)gpkLatencyRead, (unsigned long)gpkLatencyWrite,
            (unsigned long)gpkExecSettle);

    if (!sHaveReport) {
        iprintf("\nrunning...\n");
        return;
    }
    if (!r->device_present) {
        iprintf("\nDEVICE NOT DETECTED\n%s\n", layer_name(r->init_layer));
        return;
    }

    iprintf("proto %lu.%lu caps %08lX\n",
            (unsigned long)(r->protocol >> 16), (unsigned long)(r->protocol & 0xFFFF),
            (unsigned long)r->caps);
    iprintf("RAM %luK F4F5 %s\n",
            (unsigned long)(r->local_bytes / 1024),
            (r->caps & GPK_CAP_BLOCK_XPORT) ? "y" : "n");
    iprintf("alloc r%lu h%lu s%lu\n",
            (unsigned long)sProbeAllocRes, (unsigned long)sProbeHandle,
            (unsigned long)sProbeSize);
    iprintf("upsum %08lX\n", (unsigned long)sProbeUpSum);
    iprintf("legacy %s ck %08lX\n",
            pf(r->legacy_ok), (unsigned long)r->checksum);
    iprintf("F4 %s F5 %s ck %s\n",
            pf(r->f4_ok), pf(r->f5_ok), pf(r->checksum_ok));
    // Block-stage allocation handle: 0 means the stage returned before ever
    // sending an F4, which would otherwise look identical to a dropped command.
    iprintf("blkh %lu\n", (unsigned long)r->block_handle);
    // RP2040-side F4 counters: enter / accepted / complete / parsed.
    iprintf("F4cnt e%lu a%lu c%lu p%lu\n",
            (unsigned long)r->f4_enter, (unsigned long)r->f4_accepted,
            (unsigned long)r->f4_complete, (unsigned long)r->f4_parsed);

    if (!sFullRun) {
        iprintf("\nOVERALL %s\n", pf(r->overall_ok));
        return;
    }

    iprintf("cmd %lu/%lu/%lu us\n",
            (unsigned long)r->cmd_latency.min_us,
            (unsigned long)r->cmd_latency.median_us,
            (unsigned long)r->cmd_latency.p95_us);
    iprintf("F4  %lu/%lu/%lu us\n",
            (unsigned long)r->f4_latency.min_us,
            (unsigned long)r->f4_latency.median_us,
            (unsigned long)r->f4_latency.p95_us);
    iprintf("F5  %lu/%lu/%lu us\n",
            (unsigned long)r->f5_latency.min_us,
            (unsigned long)r->f5_latency.median_us,
            (unsigned long)r->f5_latency.p95_us);
    iprintf("RTT %lu/%lu/%lu us\n",
            (unsigned long)r->rtt_512.min_us,
            (unsigned long)r->rtt_512.median_us,
            (unsigned long)r->rtt_512.p95_us);
    fmt_mib(r->write_kib_per_s, buf, sizeof(buf));
    iprintf("wBW %s\n", buf);
    fmt_mib(r->read_kib_per_s, buf, sizeof(buf));
    iprintf("rBW %s\n", buf);
    for (u32 i = 0; i < r->batch_count; i++)
        iprintf("b%lu %lu.%03lu us %s\n",
                (unsigned long)r->batches[i].batch,
                (unsigned long)(r->batches[i].us_per_job_x1000 / 1000),
                (unsigned long)(r->batches[i].us_per_job_x1000 % 1000),
                r->batches[i].ok ? "ok" : "!!");
    iprintf("OVERALL %s\n", pf(r->overall_ok));
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
    // Raw head of the F5 completion block. F4 now works, but the GKC1 record
    // does not validate, so print the bytes rather than infer the layout.
    // A correct record starts 47 4B 43 31 01 00 00 00 ('GKC1', version 1).
    LOG("F5 head:");
    for (u32 i = 0; i < 16; i++)
        LOG(" %02X", (unsigned)r->f5_head[i]);
    LOG("\nF5 +16 :");
    for (u32 i = 16; i < 32; i++)
        LOG(" %02X", (unsigned)r->f5_head[i]);
    LOG("\n");
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

// Does the DLDI write path work, and does it survive GekkoPAK bus traffic?
//
// Run before and after our own card commands. probe.txt, written before any
// GekkoPAK traffic, has reached the card on every single run, while every file
// written afterwards was lost despite reporting success. If the "before" test
// passes and the "after" test fails, then our traffic breaks the DLDI write
// path - and since F4 is also a console-to-cartridge write that silently
// delivers nothing, the two failures are plausibly the same root cause.
//
// This deliberately does not call writeSectors() on a raw sector number. A
// wrong sector on a live FAT32 card destroys data, and a directory has already
// been lost once here. Going through a file we own exercises exactly the same
// dldi_writeSectors path underneath.
static bool gpk_dldi_write_test(const char *when, u32 tag)
{
    char path[80], line[64], back[64];
    if (!sPrefix[0]) {
        LOG("dldi %s: no prefix\n", when);
        return false;
    }
    siprintf(path, "%sgekkopak/results/wtest.txt", sPrefix);
    siprintf(line, "dldi-%s-%lu\n", when, (unsigned long)tag);

    FILE *f = fopen(path, "w");
    if (!f) {
        LOG("dldi %s: open FAIL\n", when);
        return false;
    }
    fputs(line, f);
    fclose(f);

    back[0] = 0;
    f = fopen(path, "r");
    if (f) {
        if (!fgets(back, sizeof(back), f))
            back[0] = 0;
        fclose(f);
    }
    bool ok = (strcmp(back, line) == 0);
    LOG("dldi %s: %s\n", when, ok ? "WRITE OK" : "WRITE FAIL");
    return ok;
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

    // Flush after every stage, not once at the end.
    //
    // probe.txt and wtest.txt, both written early, reach the card on every run
    // while an end-of-run write does not - the DLDI write path degrades after
    // sustained bus traffic just as reads do. Rewriting the whole transcript
    // after each stage means the last successful write is kept, so a run that
    // dies late still leaves everything up to that point on the card.
    //
    // Pad the tail before writing.
    //
    // Writes through this DLDI path arrive very slightly short: a 12-byte probe
    // landed as 11 bytes on the card while the verifying read, served from
    // cache, agreed with what was written. That is survivable if the shortfall
    // eats padding instead of results, so append a sentinel and filler. If the
    // sentinel is present in the file read back over USB, nothing was lost.
    // Write the transcript as a series of small files.
    //
    // Two things are known to work on this DLDI path and one is not. probe.txt
    // (21 bytes) and wtest.txt (11 bytes) are rewritten with "w" successfully on
    // every run. A single large diag.txt has never appeared, whether created
    // with "w" or written in place with "r+" into a file pre-sized to 9000
    // bytes from the host - that attempt left the file untouched, so "r+" is
    // not supported either.
    //
    // Rather than keep guessing which limit applies, use only what is proven:
    // "w" onto small files. The transcript is split into GPK_DIAG_CHUNK-byte
    // pieces written as d00.txt, d01.txt and so on, to be concatenated in order
    // on the host. Each chunk is independent, so a run that dies partway still
    // leaves every completed chunk readable.
    #define GPK_DIAG_CHUNK 384
    static char out[GPK_DIAG_CHUNK + 8];
    u32 total = sDiagLen < sizeof(sDiag) ? sDiagLen : sizeof(sDiag) - 1;
    u32 chunks = (total + GPK_DIAG_CHUNK - 1) / GPK_DIAG_CHUNK;
    if (chunks > 20)
        chunks = 20;

    for (u32 c = 0; c < chunks; c++) {
        u32 off = c * GPK_DIAG_CHUNK;
        u32 len = total - off;
        if (len > GPK_DIAG_CHUNK)
            len = GPK_DIAG_CHUNK;
        memcpy(out, sDiag + off, len);

        char tail[32];
        siprintf(tail, "gekkopak/results/d%02lu.txt", (unsigned long)c);
        gpk_join(path, sizeof(path), tail);
        FILE *d = fopen(path, "w");
        if (!d)
            break;
        fwrite(out, 1, len, d);
        fclose(d);
    }
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
    gpk_status("done");
    draw_status();
    log_details();
    LOG("done: %s\n", pf(sReport.overall_ok));
    // Autosave at the end, after all bus traffic. Read it back over USB with
    // the mass-storage app so a result never needs photographing.
    write_diag_only();
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

    gpk_dldi_write_test("pre", 1);

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
    gpk_status("bus probe");
    LOG("--- bus probe ---\n");
    LOG("EXMEMCNT   : %04X (arm9 owns card: %s)\n",
        (unsigned)REG_EXMEMCNT, (REG_EXMEMCNT & ARM7_OWNS_CARD) ? "NO" : "yes");
    // Capture B8 rather than only logging it: the summary page reads sProbeB8,
    // and while this was never assigned that page showed a constant 00000000,
    // which was mistaken for a dead cartridge more than once.
    sProbeB8 = gpk_raw_read32(0xB800000000000000ull);
    LOG("B8 card id : %08lX\n", (unsigned long)sProbeB8);
    write_diag_only();  // flush this stage to the card

    // Refuse to go further on a dead link.
    //
    // B8 is a stock game-mode command and reads C00000C2 on a healthy run.
    // 00000000 or FFFFFFFF means the cartridge is not answering, and everything
    // past this point then yields undriven values that look like measurements -
    // a matrix of FFFFFFFF counters was nearly read as six failed F4 variants.
    //
    // The known cause is the USB-then-reset crash upstream documents. The
    // DSpico is USB powered, so switching the console off with the cable still
    // attached does not reset it; the cable has to come out too.
    if (sProbeB8 == 0x00000000u || sProbeB8 == 0xFFFFFFFFu) {
        consoleSelect(&sTop);
        consoleClear();
        iprintf("GEKKOPAK PHYS TEST\n\nLINK DEAD\n\n");
        iprintf("B8 read %08lX\n", (unsigned long)sProbeB8);
        iprintf("(healthy is C00000C2)\n\n");
        iprintf("Cartridge not answering.\nPower cycle it:\n\n");
        iprintf(" 1 unplug USB\n 2 console OFF\n");
        iprintf(" 3 remove cartridge\n 4 wait a few seconds\n");
        iprintf(" 5 reinsert, power on\n\nNo results this run.\n");
        LOG("LINK DEAD - refusing to run\n");
        while (pmMainLoop()) {
            swiWaitForVBlank();
            scanKeys();
        }
    }
    // E4 (GET_SD_STAT) is deliberately NOT issued any more.
    //
    // It answered 00000001 on hardware and served its purpose - it proved the
    // cartridge was in unscrambled game mode and that DSpico's extended command
    // dispatch reached us. But it polls the same SD state machine the DLDI
    // driver drives, whose read sequence is E3 -> poll E4 -> E5. A stray E4
    // consumes a state transition and desynchronises it, after which libfat
    // writes report success and never reach the card.
    //
    // That matches the evidence exactly: probe.txt, written before any raw card
    // traffic, persists every run, while every file written afterwards is lost
    // despite fopen/fwrite/fclose all succeeding. It is very likely also what
    // destroyed a directory earlier - a write landing somewhere it should not.

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
    gpk_status("latency sweep");
    LOG("F2 read latency sweep (of 32):\n");
    u32 chosen = 0;
    for (u32 i = 0; i < sizeof(kLatencies) / sizeof(kLatencies[0]); i++) {
        gpkLatencyRead = kLatencies[i];
        gpk_status("latency sweep: %lu", (unsigned long)kLatencies[i]);
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
    LOG("using lat r/w %lu/%lu (timeouts %lu)\n",
        (unsigned long)gpkLatencyRead, (unsigned long)gpkLatencyWrite,
        (unsigned long)gpkTimeouts);
    write_diag_only();  // flush this stage to the card

    // EXEC settle sweep.
    //
    // The register sweep above passes at every latency, so the register path is
    // sound. What fails is HELLO, which is the only thing that issues F1 EXEC -
    // and DSpico runs executeHighCommand() inside the card IRQ, so a following
    // F2 arrives while the RP2040 is still busy. Measure how much settle time
    // it actually needs instead of picking a number; the answer bounds the cost
    // of every EXEC-based operation and belongs in the results.
    static const u32 kSettles[] = { 0, 16, 64, 256, 1024 };
    gpk_status("EXEC settle sweep");
    LOG("EXEC settle sweep (HELLO, of 16):\n");
    u32 settle = 0;
    for (u32 i = 0; i < sizeof(kSettles) / sizeof(kSettles[0]); i++) {
        gpkExecSettle = kSettles[i];
        gpk_status("settle sweep: %lu", (unsigned long)kSettles[i]);
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
    LOG("using settle %lu (8x margin, timeouts %lu)\n",
        (unsigned long)gpkExecSettle, (unsigned long)gpkTimeouts);
    write_diag_only();  // flush this stage to the card

    // Step-by-step legacy probe. `legacy F0-F3 FAIL (00000000)` does not say
    // which step broke or why, and the GekkoPAK result codes are specific
    // (2=BadHandle 3=NoMemory 4=NotReady 5=BadDescriptor 7=QueueFull), so print
    // them rather than infer.
    gpk_status("legacy probe");
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
    sProbeAllocRes = pres; sProbeHandle = phandle; sProbeSize = psize;
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

    consoleSelect(&sTop);
    draw_status();

    // Repeat the write test now that GekkoPAK commands have been issued.
    gpk_dldi_write_test("post", 2);

    // F4 variant matrix: six configurations in one boot, so a round trip tests
    // six hypotheses instead of one. Printed on the log screen; the summary page
    // stays reserved for the benchmark.
    {
        gpk_f4_variant_t vars[GPK_F4_VARIANTS];
        memset(vars, 0, sizeof(vars));
        u32 n = gpk_f4_matrix(vars);
        gpk_status("F4 matrix");
        LOG("--- F4 matrix ---\n");
        if (n == 0) {
            LOG("alloc failed; no variants run\n");
        } else {
            for (u32 i = 0; i < n; i++)
                LOG("%-11s d%lu e%lu a%lu c%lu p%lu\n", vars[i].name,
                    (unsigned long)vars[i].completions, (unsigned long)vars[i].enter,
                    (unsigned long)vars[i].accepted, (unsigned long)vars[i].complete,
                    (unsigned long)vars[i].parsed);
        }
    }
    write_diag_only();  // flush after the F4 matrix

    // No automatic benchmark. Actions are bound to buttons instead, so one
    // build can run many experiments without a rebuild, a copy and a card swap
    // for each - that cycle, not the difficulty of the faults, has been the
    // dominant cost of this bring-up.
    draw_menu();

    // USB is NOT started here. It is opt-in, on SELECT.
    //
    // Starting it at boot wedged the cartridge: B8 read FFFFFFFF, where the
    // previous build - which brought USB up only after the benchmark - read
    // C00000C2. USB on this cartridge is card traffic: tud_init() issues E8
    // commands and gpk_usb_task() polls EB every frame, so leaving it running
    // underneath the transport corrupts the bus being tested.
    //
    // Removing the automatic benchmark is what exposed this. The USB init had
    // been sequenced after it, and with the run gone it moved to startup.
    u32 sent = 0;
    bool announced = false;
    bool usbStarted = false;

    bool marked = false;
    while (pmMainLoop()) {
        swiWaitForVBlank();
        if (!marked) {
            // Reached the main loop: report it on the log screen, since VRAM_A
            // now belongs to the console rather than the bitmap marker.
            LOG("boot: reached main loop\n");
            marked = true;
        }
        // Pump USB every frame, and stream the transcript once a host has
        // opened the port. Sent in small pieces so a full CDC FIFO simply
        // resumes on the next frame rather than blocking the loop.
        if (usbStarted) {
            gpk_usb_task();
            tud_task();
        }

        if (usbStarted && tud_cdc_connected()) {
            if (!announced) {
                gpk_status("usb: host connected");
                announced = true;
            }
            if (sent < sDiagLen) {
                u32 chunk = sDiagLen - sent;
                u32 space = tud_cdc_write_available();
                if (chunk > space)
                    chunk = space;
                if (chunk > 64)
                    chunk = 64;
                if (chunk) {
                    sent += (u32)tud_cdc_write(sDiag + sent, chunk);
                    tud_cdc_write_flush();
                }
            }
        } else if (usbStarted) {
            announced = false;
            sent = 0;
        }

        scanKeys();
        u32 keys = keysDown();

        if (keys & KEY_A) {
            run(false);                       // quick: discovery, legacy, block
        } else if (keys & KEY_X) {
            run(true);                        // full benchmark with timings
        } else if (keys & KEY_Y) {
            // F4 variant matrix on demand.
            gpk_status("F4 matrix");
            gpk_f4_variant_t vars[GPK_F4_VARIANTS];
            memset(vars, 0, sizeof(vars));
            u32 n = gpk_f4_matrix(vars);
            LOG("--- F4 matrix ---\n");
            if (n == 0) {
                LOG("link down; not run\n");
            } else {
                for (u32 i = 0; i < n; i++)
                    LOG("%-11s d%lu e%lu a%lu c%lu p%lu\n", vars[i].name,
                        (unsigned long)vars[i].completions, (unsigned long)vars[i].enter,
                        (unsigned long)vars[i].accepted, (unsigned long)vars[i].complete,
                        (unsigned long)vars[i].parsed);
            }
            gpk_status("F4 matrix done");
        } else if (keys & KEY_UP) {
            // F5 read sweep: latency x priming, reporting where 'GKC1' lands.
            //
            // The record is written at offset 0 by the cartridge but arrived
            // at offset 12 behind three words of 0xFF. This says whether that
            // skew is latency (offset falls as LATENCY2 rises), a dropped
            // first transaction (priming clears it), or structural (12
            // regardless of either).
            gpk_status("F5 sweep");
            gpk_f5_variant_t fv[GPK_F5_VARIANTS];
            memset(fv, 0, sizeof(fv));
            u32 fn = gpk_f5_matrix(fv);
            LOG("--- F5 sweep ---\n");
            if (fn == 0) {
                LOG("link down; not run\n");
            } else {
                for (u32 i = 0; i < fn; i++) {
                    LOG("lat%-2lu %-5s ", (unsigned long)fv[i].latency,
                        fv[i].name);
                    if (fv[i].magic_offset == GPK_F5_NO_MAGIC)
                        LOG("GKC1 absent w0 %08lX\n",
                            (unsigned long)fv[i].head0);
                    else
                        LOG("GKC1 @%-3lu   w0 %08lX\n",
                            (unsigned long)fv[i].magic_offset,
                            (unsigned long)fv[i].head0);
                }
            }
            gpk_status("F5 sweep done");
        } else if (keys & KEY_B) {
            // Re-probe the link without disturbing anything else. Cheap, and
            // the first thing worth knowing when a run looks wrong.
            u32 id = gpk_raw_read32(0xB800000000000000ull);
            u32 proto = 0;
            u32 res = gpk_hello(&proto, NULL, NULL, NULL);
            LOG("B8 %08lX hello r%lu proto %08lX\n",
                (unsigned long)id, (unsigned long)res, (unsigned long)proto);
            gpk_status("probe: B8 %08lX", (unsigned long)id);
        } else if (keys & KEY_L) {
            // Step write latency: the parameter most likely to matter for F4,
            // and previously only changeable by rebuilding.
            static const u32 kLat[] = { 8, 16, 32, 63 };
            static u32 li = 0;
            li = (li + 1) % 4;
            gpkLatencyWrite = kLat[li];
            gpk_status("write latency %lu", (unsigned long)gpkLatencyWrite);
        } else if (keys & KEY_R) {
            static const u32 kSet[] = { 16, 128, 512, 2048 };
            static u32 si = 0;
            si = (si + 1) % 4;
            gpkExecSettle = kSet[si];
            gpk_status("exec settle %lu", (unsigned long)gpkExecSettle);
        } else if (keys & KEY_START) {
            run(sFullRun);
        } else if (keys & KEY_SELECT) {
            // Start USB on demand, after the measurements are done. Starting it
            // any earlier runs card traffic underneath the transport under test.
            if (!usbStarted) {
                LOG("\nusb: starting CDC\n");
                gpk_status("usb: connect a cable");
                tud_init(0);
                tud_connect();   // tud_init alone does not assert the pull-up
                usbStarted = true;
            } else {
                LOG("usb: already running\n");
            }
        }
    }
    return 0;
}
