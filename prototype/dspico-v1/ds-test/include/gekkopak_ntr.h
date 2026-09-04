// GekkoPAK v1 DS-side NTR transport.
//
// This header owns the host-side wire serialization for the GekkoPAK F0-F5
// command set. Per docs/NTR_WIRE_V1.md the boundary is deliberately explicit:
//
//   command header : 8 bytes, big-endian, byte 0 is the opcode
//                    OP 47 4B II VV VV VV VV
//   payload data   : byte-transparent in both directions
//
// The asymmetry is a property of the bus, not of the descriptor ABI:
// DSpico's PIO uses a left-shifting input register (so the first command byte
// lands in bits 31:24 of cmd0) but a right-shifting output register, and its
// DS->cart payload path applies `rev` before storing. The net effect is that
// payload bytes appear in DSpico SRAM in exactly the order the DS emitted them,
// while the command word must be assembled MSB-first by hand.
#ifndef GEKKOPAK_NTR_H
#define GEKKOPAK_NTR_H

#include <nds.h>

#ifdef __cplusplus
extern "C" {
#endif

// Wire opcodes (unscrambled game mode).
#define GPK_OP_WRITE_REG    0xF0
#define GPK_OP_EXEC         0xF1
#define GPK_OP_READ_REG     0xF2
#define GPK_OP_PAYLOAD_WORD 0xF3
#define GPK_OP_WRITE_BLOCK  0xF4
#define GPK_OP_READ_BLOCK   0xF5

// Stage register indices (F0 write / F2 read).
#define GPK_REG_ARG0        0
#define GPK_REG_ARG1        1
#define GPK_REG_ARG2        2
#define GPK_REG_ARG3        3
#define GPK_REG_PAYLOAD_LEN 4
#define GPK_REG_RESULT      5
#define GPK_REG_OUT0        6
#define GPK_REG_OUT1        7
#define GPK_REG_OUT2        8
#define GPK_REG_OUT3        9
#define GPK_REG_EVENT_DEPTH 0xFE

// High-level commands (F1 EXEC index).
#define GPK_CMD_HELLO    1
#define GPK_CMD_GETCAPS  2
#define GPK_CMD_ALLOC    3
#define GPK_CMD_UPLOAD   4
#define GPK_CMD_SUBMIT   5
#define GPK_CMD_POLL     6
#define GPK_CMD_COLLECT  7
#define GPK_CMD_FREE     8
#define GPK_CMD_COMPLETE 9

// Result codes.
#define GPK_OK             0
#define GPK_ERR_BADCOMMAND 1
#define GPK_ERR_BADHANDLE  2
#define GPK_ERR_NOMEMORY   3
#define GPK_ERR_NOTREADY   4
#define GPK_ERR_BADDESC    5
#define GPK_ERR_BADBLOCK   6
#define GPK_ERR_QUEUEFULL  7

#define GPK_PROTOCOL_V1     0x00010000u
#define GPK_CAP_BLOCK_XPORT (1u << 4)

#define GPK_BLOCK_BYTES      512
#define GPK_DESCRIPTOR_BYTES 64
#define GPK_COMPLETION_BYTES 64
#define GPK_MAX_BATCH        (GPK_BLOCK_BYTES / GPK_DESCRIPTOR_BYTES)

#define GPK_DESC_MAGIC 0x31444B47u // 'GKD1'
#define GPK_COMP_MAGIC 0x31434B47u // 'GKC1'
#define GPK_BLOCK_VERSION 1
#define GPK_SUBMIT_OPCODE 1
#define GPK_FLAG_INLINE_INPUT (1u << 0)

// 64-byte submission descriptor. Little-endian, as emitted by the ARM guest.
typedef struct {
    u32 magic;
    u16 version;
    u16 opcode;
    u32 sequence;
    u32 flags;
    u32 input_handle;
    u32 input_offset;
    u32 input_length;
    u32 output_handle;
    u32 output_offset;
    u32 output_length;
    u32 kernel_id;
    u32 work_units;
    u32 arg0;
    u32 arg1;
    u32 arg2;
    u32 arg3;
} gpk_descriptor_t;

// 64-byte completion record.
typedef struct {
    u32 magic;
    u16 version;
    u16 status;
    u32 sequence;
    u32 job_handle;
    u32 modeled_us;
    u32 speedup_x1000;
    u32 checksum;
    u32 software_us;
    u32 output_length;
    u32 transport_us_x1000;
    u32 batch_size;
    u32 reserved[5];
} gpk_completion_t;

// Bus latency, in card-clock cycles, inserted before the data phase.
// docs/commands.md requires >=4 for cart->console and >=8 for console->cart.
// These are the knobs that dominate measured per-transaction cost; they are
// reported alongside every benchmark so a result is always reproducible.
extern u32 gpkLatencyRead;
extern u32 gpkLatencyWrite;

// Free-running 33.513982 MHz tick counter (cascaded timers 0/1).
void gpk_timer_init(void);
u32  gpk_ticks(void);
#define GPK_TICKS_PER_US 33.513982f
static inline u32 gpk_ticks_to_us(u32 t) { return (u32)(t / GPK_TICKS_PER_US); }

// Take slot-1 for the ARM9 and put the cartridge into unscrambled game mode.
// Returns 0 on success, or a GPK_LAYER_* code identifying the first layer that
// failed so a failure is never reported as just "hardware test failed".
#define GPK_LAYER_NONE       0
#define GPK_LAYER_CARD_OWNER 1
#define GPK_LAYER_UNSCRAMBLE 2
#define GPK_LAYER_HELLO      3
#define GPK_LAYER_PROTOCOL   4
int gpk_transport_init(void);

// Raw command primitives. `index` is command byte 3, `word` is bytes 4..7.
void gpk_cmd_none(u8 opcode, u8 index, u32 word);
u32  gpk_cmd_read32(u8 opcode, u8 index, u32 word);
void gpk_cmd_read_block(u8 opcode, u8 index, u32 word, void *dst);
void gpk_cmd_write_block(u8 opcode, u8 index, u32 word, const void *src);

// F0/F2 stage register access.
static inline void gpk_write_reg(u8 index, u32 value) { gpk_cmd_none(GPK_OP_WRITE_REG, index, value); }
static inline u32  gpk_read_reg(u8 index)             { return gpk_cmd_read32(GPK_OP_READ_REG, index, 0); }
// F1 EXEC needs a settle gap before the next command.
//
// DSpico runs executeHighCommand() inside the card IRQ handler, so issuing the
// following F2 READ_REG immediately means the RP2040 is still busy and its PIO
// response arrives late - the DS then clocks out FFFFFFFF. F0 is a single
// store and always keeps up, which is why the register sweep passes while
// HELLO, ALLOC and UPLOAD (all EXEC commands) fail.
extern u32 gpkExecSettle;
// Count of bus transfers that never completed; nonzero means a real bus fault.
extern u32 gpkTimeouts;
void gpk_exec(u8 command);
static inline u32  gpk_event_depth(void)              { return gpk_read_reg(GPK_REG_EVENT_DEPTH); }

// F3 legacy payload staging: writes one 32-bit word at wordIndex*4.
static inline void gpk_payload_word(u8 wordIndex, u32 value) { gpk_cmd_none(GPK_OP_PAYLOAD_WORD, wordIndex, value); }

// F4: submit a 512-byte block; `meaningfulBytes` is the descriptor region length.
static inline void gpk_write_block(const void *block, u32 meaningfulBytes) {
    gpk_cmd_write_block(GPK_OP_WRITE_BLOCK, 0, meaningfulBytes, block);
}
// F5: read the 512-byte completion block. Offset must be 0.
static inline void gpk_read_block(void *block, u32 meaningfulBytes) {
    gpk_cmd_read_block(GPK_OP_READ_BLOCK, 1, (meaningfulBytes << 16), block);
}

// Raw 8-byte command with a 4-byte read phase, using the same bus settings as
// every other transaction. Exists so the test can issue DSpico's own commands
// (B8 read-id, E4 sd-status) and tell "the bus is wrong" apart from "the
// GekkoPAK handlers are not answering".
u32 gpk_raw_read32(u64 command);

// Convenience wrappers around the F0/F1/F2 legacy path.
u32 gpk_alloc(u32 bytes, u32 *sizeOut);
u32 gpk_hello(u32 *protocol, u32 *caps, u32 *localBytes, u32 *transport);

#ifdef __cplusplus
}
#endif
#endif
