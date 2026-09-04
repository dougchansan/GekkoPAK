#include "gekkopak_ntr.h"
#include <string.h>

// Card bus registers, named as in the DSpico DLDI driver (LNH-team/dspico-dldi
// source/card.h) rather than libnds, because that driver is the reference
// implementation for talking to this cartridge and matching it exactly removes
// a whole class of guesswork.
#define REG_MCCNT0 (*(vu16 *)0x040001A0)
#define REG_MCCNT1 (*(vu32 *)0x040001A4)
#define REG_MCCMD0 (*(vu32 *)0x040001A8)
#define REG_MCD1   (*(vu32 *)0x04100010)

#define MCCNT0_MODE_MASK    (1 << 13)
#define MCCNT0_MODE_ROM     (0 << 13)
#define MCCNT0_ENABLE       (1 << 15)

#define MCCNT1_LATENCY1(x)           (x)
#define MCCNT1_READ_DATA_DESCRAMBLE  (1 << 13)
#define MCCNT1_CLOCK_SCRAMBLER       (1 << 14)
#define MCCNT1_LATENCY2(x)           (((x) << 16) & 0x3F0000)
#define MCCNT1_CMD_SCRAMBLE          (1 << 22)
#define MCCNT1_DATA_READY            (1 << 23)
#define MCCNT1_LEN_0                 (0 << 24)
#define MCCNT1_LEN_512               (1 << 24)
#define MCCNT1_LEN_4                 (7 << 24)
#define MCCNT1_CLK_6_7_MHZ           (0 << 27)
#define MCCNT1_RESET_OFF             (1 << 29)
#define MCCNT1_DIR_READ              (0u << 30)
#define MCCNT1_DIR_WRITE             (1u << 30)
#define MCCNT1_ENABLE                (1u << 31)

// Latency, in card clocks, inserted before the data phase. docs/commands.md
// asks for >=4 on cart->console and >=8 on console->cart, and the DLDI driver
// uses exactly those in LATENCY2 with LATENCY1 left at zero. Reported with
// every benchmark so a measurement is reproducible.
u32 gpkLatencyRead = 4;
u32 gpkLatencyWrite = 8;

// KEY2 command scrambling stays ENABLED for every transaction.
//
// This is the detail that made a first attempt fail with "HELLO no answer".
// docs/commands.md says to zero the scrambler seeds so the scrambler emits
// zeros, and every DSpico DLDI transfer then still sets CMD_SCRAMBLE and
// CLOCK_SCRAMBLER. The console and the cartridge each advance a scrambler ring
// per command, so the two only stay in step if the DS keeps clocking its ring.
// Clearing these bits mid-session desynchronises the link and every subsequent
// command decodes to garbage.
//
// There is also no FC (DISABLE_SCRAMBLING) transition here. By the time this
// runs, Pico Loader has already put the cartridge into unscrambled game mode -
// which is why the DLDI driver issues its E3/E4/E5 commands directly and never
// sends FC either.
#define GPK_SCRAMBLE_BITS (MCCNT1_CMD_SCRAMBLE | MCCNT1_CLOCK_SCRAMBLER)

void gpk_timer_init(void)
{
    TIMER_CR(0) = 0;
    TIMER_CR(1) = 0;
    TIMER_DATA(0) = 0;
    TIMER_DATA(1) = 0;
    TIMER_CR(0) = TIMER_DIV_1 | TIMER_ENABLE;
    TIMER_CR(1) = TIMER_CASCADE | TIMER_ENABLE;
}

u32 gpk_ticks(void)
{
    // Re-read the low half to detect a carry landing between the two reads.
    u32 lo = TIMER_DATA(0);
    u32 hi = TIMER_DATA(1);
    u32 lo2 = TIMER_DATA(0);
    if (lo2 < lo) {
        lo = lo2;
        hi = TIMER_DATA(1);
    }
    return (hi << 16) | lo;
}

// Assemble the 8-byte command. GekkoPAK owns this serialization and nothing
// below it depends on the ordering.
//
// The command is written as a big-endian u64 and byte-swapped into the
// register pair, so command byte 0 (the opcode) goes out first - matching
// card_romSetCmd() in the DLDI driver and DSpico's `cmd0 >> 24` dispatch.
static inline void gpk_write_command(u8 opcode, u8 index, u32 word)
{
    u64 cmd = ((u64)opcode << 56) |
              ((u64)0x47   << 48) | // 'G'
              ((u64)0x4B   << 40) | // 'K'
              ((u64)index  << 32) |
              (u64)word;
    *(vu64 *)&REG_MCCMD0 = __builtin_bswap64(cmd);
}

static inline void gpk_start(u32 settings)
{
    REG_MCCNT0 = (REG_MCCNT0 & ~MCCNT0_MODE_MASK) | MCCNT0_MODE_ROM | MCCNT0_ENABLE;
    REG_MCCNT1 = MCCNT1_ENABLE | settings;
}

static inline bool gpk_data_ready(void) { return REG_MCCNT1 & MCCNT1_DATA_READY; }
static inline bool gpk_busy(void)       { return REG_MCCNT1 & MCCNT1_ENABLE; }

void gpk_cmd_none(u8 opcode, u8 index, u32 word)
{
    gpk_write_command(opcode, index, word);
    gpk_start(MCCNT1_DIR_READ | MCCNT1_RESET_OFF | MCCNT1_CLK_6_7_MHZ | MCCNT1_LEN_0 |
              GPK_SCRAMBLE_BITS | MCCNT1_READ_DATA_DESCRAMBLE |
              MCCNT1_LATENCY2(0) | MCCNT1_LATENCY1(0));
    while (gpk_busy()) { }
}

u32 gpk_cmd_read32(u8 opcode, u8 index, u32 word)
{
    u32 value = 0;
    gpk_write_command(opcode, index, word);
    gpk_start(MCCNT1_DIR_READ | MCCNT1_RESET_OFF | MCCNT1_CLK_6_7_MHZ | MCCNT1_LEN_4 |
              GPK_SCRAMBLE_BITS | MCCNT1_READ_DATA_DESCRAMBLE |
              MCCNT1_LATENCY2(gpkLatencyRead) | MCCNT1_LATENCY1(0));
    do {
        if (gpk_data_ready())
            value = REG_MCD1;
    } while (gpk_busy());
    return value;
}

void gpk_cmd_read_block(u8 opcode, u8 index, u32 word, void *dst)
{
    u32 *out = (u32 *)dst;
    u32 *end = out + (GPK_BLOCK_BYTES / 4);
    gpk_write_command(opcode, index, word);
    gpk_start(MCCNT1_DIR_READ | MCCNT1_RESET_OFF | MCCNT1_CLK_6_7_MHZ | MCCNT1_LEN_512 |
              GPK_SCRAMBLE_BITS |
              MCCNT1_LATENCY2(gpkLatencyRead) | MCCNT1_LATENCY1(0));
    do {
        if (gpk_data_ready()) {
            u32 w = REG_MCD1;
            if (out < end)
                *out++ = w;
        }
    } while (gpk_busy());
}

void gpk_cmd_write_block(u8 opcode, u8 index, u32 word, const void *src)
{
    const u32 *in = (const u32 *)src;
    const u32 *end = in + (GPK_BLOCK_BYTES / 4);
    u32 data = 0;
    gpk_write_command(opcode, index, word);
    gpk_start(MCCNT1_DIR_WRITE | MCCNT1_RESET_OFF | MCCNT1_CLK_6_7_MHZ | MCCNT1_LEN_512 |
              GPK_SCRAMBLE_BITS | MCCNT1_READ_DATA_DESCRAMBLE |
              MCCNT1_LATENCY2(gpkLatencyWrite) | MCCNT1_LATENCY1(0));
    do {
        if (gpk_data_ready()) {
            if (in < end)
                data = *in++;
            REG_MCD1 = data;
        }
    } while (gpk_busy());
}

// swiDelay units inserted after every F1 EXEC. Measured by the startup sweep
// rather than guessed; reported with results so a run is reproducible.
u32 gpkExecSettle = 256;

void gpk_exec(u8 command)
{
    gpk_cmd_none(GPK_OP_EXEC, command, 0);
    if (gpkExecSettle)
        swiDelay(gpkExecSettle);

    // Discard one read.
    //
    // Measured on hardware: the first F2 after an F1 EXEC is always lost. A
    // burst read of RESULT and OUT0 straight after an ALLOC returned
    //
    //   alloc  res 00FFFFFF  h FFFFFFFF        <- first read of each register
    //   res x4 00000000 00000000 00000000 00000000
    //   out0x4 00000001 00000001 00000001 00000001
    //
    // so the values are correct and stable and only the first transaction after
    // the EXEC comes back undriven. It is not a settle-duration problem - 128
    // units of delay did not help - the command itself is dropped, because the
    // RP2040 is still finishing the EXEC handler when the next CEB edge arrives
    // and its PIO never captures that command. One throwaway transaction costs
    // a few microseconds and makes every EXEC-based operation reliable.
    (void)gpk_read_reg(GPK_REG_RESULT);
}

u32 gpk_raw_read32(u64 command)
{
    u32 value = 0;
    *(vu64 *)&REG_MCCMD0 = __builtin_bswap64(command);
    gpk_start(MCCNT1_DIR_READ | MCCNT1_RESET_OFF | MCCNT1_CLK_6_7_MHZ | MCCNT1_LEN_4 |
              GPK_SCRAMBLE_BITS | MCCNT1_READ_DATA_DESCRAMBLE |
              MCCNT1_LATENCY2(gpkLatencyRead) | MCCNT1_LATENCY1(0));
    do {
        if (gpk_data_ready())
            value = REG_MCD1;
    } while (gpk_busy());
    return value;
}

u32 gpk_hello(u32 *protocol, u32 *caps, u32 *localBytes, u32 *transport)
{
    gpk_exec(GPK_CMD_HELLO);
    u32 result = gpk_read_reg(GPK_REG_RESULT);
    if (protocol)   *protocol   = gpk_read_reg(GPK_REG_OUT0);
    if (caps)       *caps       = gpk_read_reg(GPK_REG_OUT1);
    if (localBytes) *localBytes = gpk_read_reg(GPK_REG_OUT2);
    if (transport)  *transport  = gpk_read_reg(GPK_REG_OUT3);
    return result;
}

u32 gpk_alloc(u32 bytes, u32 *sizeOut)
{
    gpk_write_reg(GPK_REG_ARG0, bytes);
    gpk_exec(GPK_CMD_ALLOC);
    if (gpk_read_reg(GPK_REG_RESULT) != GPK_OK)
        return 0;
    if (sizeOut)
        *sizeOut = gpk_read_reg(GPK_REG_OUT1);
    return gpk_read_reg(GPK_REG_OUT0);
}

int gpk_transport_init(void)
{
    // Claim slot-1 for the ARM9 by hand rather than via ntrcardOpen(): the
    // calico helper would run its own card bring-up, and this test must own
    // every transaction that reaches the bus.
    REG_EXMEMCNT &= ~ARM7_OWNS_CARD;
    if ((REG_EXMEMCNT & ARM7_OWNS_CARD) != 0)
        return GPK_LAYER_CARD_OWNER;

    gpk_timer_init();

    u32 protocol = 0;
    if (gpk_hello(&protocol, NULL, NULL, NULL) != GPK_OK)
        return GPK_LAYER_HELLO;
    if (protocol != GPK_PROTOCOL_V1)
        return GPK_LAYER_PROTOCOL;
    return GPK_LAYER_NONE;
}
