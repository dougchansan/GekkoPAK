#include "gekkopak_ntr.h"
#include <string.h>

// The gamecard 4-byte data port. card.h only declares the read direction;
// console->cart transfers use the same address with CARD_WR set in ROMCTRL.
#define GPK_CARD_DATA (*(vu32 *)0x04100010)

// docs/commands.md: >=4 latency cycles for cart->console, >=8 for console->cart.
// Kept generous by default; the benchmark reports the values actually used.
u32 gpkLatencyRead = 8;
u32 gpkLatencyWrite = 16;

// KEY2 command scrambling is still active in game mode when the cartridge was
// brought up by the console's own boot path. gpk_transport_init() determines
// empirically whether the FC transition needs it, and records the answer here.
static u32 sSecCmdFlag = 0;

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

// Assemble the 8-byte command MSB-first. This is the one place where GekkoPAK
// owns the bus byte order; nothing below this line depends on it.
static inline void gpk_write_command(u8 opcode, u8 index, u32 word)
{
    REG_CARD_COMMAND[0] = opcode;
    REG_CARD_COMMAND[1] = 0x47; // 'G'
    REG_CARD_COMMAND[2] = 0x4B; // 'K'
    REG_CARD_COMMAND[3] = index;
    REG_CARD_COMMAND[4] = (u8)(word >> 24);
    REG_CARD_COMMAND[5] = (u8)(word >> 16);
    REG_CARD_COMMAND[6] = (u8)(word >> 8);
    REG_CARD_COMMAND[7] = (u8)word;
}

static inline void gpk_begin(u32 romctrl)
{
    REG_AUXSPICNT = CARD_ENABLE;
    REG_ROMCTRL = romctrl | CARD_ACTIVATE | CARD_nRESET;
}

void gpk_cmd_none(u8 opcode, u8 index, u32 word)
{
    gpk_write_command(opcode, index, word);
    gpk_begin(sSecCmdFlag | CARD_DELAY1(gpkLatencyRead) | CARD_BLK_SIZE(0));
    while (REG_ROMCTRL & CARD_BUSY) {
        // No data phase; the block-size-0 transfer still clocks the command out.
    }
}

u32 gpk_cmd_read32(u8 opcode, u8 index, u32 word)
{
    u32 value = 0;
    gpk_write_command(opcode, index, word);
    // BLK_SIZE(7) is the 4-byte transfer encoding.
    gpk_begin(sSecCmdFlag | CARD_DELAY1(gpkLatencyRead) | CARD_BLK_SIZE(7));
    do {
        if (REG_ROMCTRL & CARD_DATA_READY)
            value = GPK_CARD_DATA;
    } while (REG_ROMCTRL & CARD_BUSY);
    return value;
}

void gpk_cmd_read_block(u8 opcode, u8 index, u32 word, void *dst)
{
    u32 *out = (u32 *)dst;
    u32 *end = out + (GPK_BLOCK_BYTES / 4);
    gpk_write_command(opcode, index, word);
    // BLK_SIZE(1) == 0x100 << 1 == 512 bytes.
    gpk_begin(sSecCmdFlag | CARD_DELAY1(gpkLatencyRead) | CARD_BLK_SIZE(1));
    do {
        if (REG_ROMCTRL & CARD_DATA_READY) {
            u32 w = GPK_CARD_DATA;
            if (out < end)
                *out++ = w;
        }
    } while (REG_ROMCTRL & CARD_BUSY);
}

void gpk_cmd_write_block(u8 opcode, u8 index, u32 word, const void *src)
{
    const u32 *in = (const u32 *)src;
    const u32 *end = in + (GPK_BLOCK_BYTES / 4);
    gpk_write_command(opcode, index, word);
    gpk_begin(sSecCmdFlag | CARD_WR | CARD_DELAY1(gpkLatencyWrite) | CARD_BLK_SIZE(1));
    do {
        if (REG_ROMCTRL & CARD_DATA_READY) {
            u32 w = (in < end) ? *in++ : 0;
            GPK_CARD_DATA = w;
        }
    } while (REG_ROMCTRL & CARD_BUSY);
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

// Try the FC (DISABLE_SCRAMBLING) transition and confirm it took effect by
// reading back the protocol version. Returns true when GekkoPAK answers.
static bool gpk_try_unscramble(u32 secCmdFlag)
{
    sSecCmdFlag = secCmdFlag;
    // FC is a game-mode command with no payload; DSpico switches to
    // unscrambled game mode when it sees the second command word.
    REG_CARD_COMMAND[0] = 0xFC;
    for (int i = 1; i < 8; i++)
        REG_CARD_COMMAND[i] = 0;
    gpk_begin(secCmdFlag | CARD_DELAY1(gpkLatencyRead) | CARD_BLK_SIZE(0));
    while (REG_ROMCTRL & CARD_BUSY) { }

    // From here on the link must be unscrambled on both sides.
    sSecCmdFlag = 0;
    u32 protocol = 0;
    if (gpk_hello(&protocol, NULL, NULL, NULL) != GPK_OK)
        return false;
    return protocol == GPK_PROTOCOL_V1;
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

    // The console's boot path may or may not have left KEY2 command scrambling
    // enabled. Try the scrambled transition first, then the plain one.
    if (gpk_try_unscramble(CARD_SEC_CMD))
        return GPK_LAYER_NONE;
    if (gpk_try_unscramble(0))
        return GPK_LAYER_NONE;

    // Distinguish "no answer at all" from "answered with the wrong protocol".
    sSecCmdFlag = 0;
    u32 protocol = 0;
    if (gpk_hello(&protocol, NULL, NULL, NULL) != GPK_OK)
        return GPK_LAYER_HELLO;
    if (protocol != GPK_PROTOCOL_V1)
        return GPK_LAYER_PROTOCOL;
    return GPK_LAYER_UNSCRAMBLE;
}
