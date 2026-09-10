// Host link over the cartridge's own USB port. See gekkopakLink.h.

#include "gekkopakLink.h"

#include <string.h>

#include "gekkopakNtr.h"
#include "gekkopakTrace.h"
#include "pico/bootrom.h"
#include "hardware/irq.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/rosc.h"
#include "hardware/structs/scb.h"
#include "hardware/xosc.h"
#include "powerSaving.h"
#include "tusb.h"

namespace {

constexpr u32 kBlockBytes = 512;

// --------------------------------------------------------------------------
// USB descriptors
// --------------------------------------------------------------------------
//
// CDC ACM rather than a vendor class, so Windows binds usbser.sys on its own
// and the harness needs no driver install. 0x2E8A is Raspberry Pi's vendor ID
// and 0x000A its CDC product ID -- the same pair the Pico SDK's own stdio_usb
// uses, which is what this device is.

constexpr u16 kVendorId = 0x2E8A;
constexpr u16 kProductId = 0x000A;

const tusb_desc_device_t kDeviceDescriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Declared at the device level, as a CDC composite must be, or Windows
    // pairs the two interfaces as separate devices.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = kVendorId,
    .idProduct = kProductId,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01};

enum { kItfCdcComm = 0, kItfCdcData, kItfCount };

#define GPK_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

constexpr u8 kEpCdcNotify = 0x81;
constexpr u8 kEpCdcOut = 0x02;
constexpr u8 kEpCdcIn = 0x82;

const u8 kConfigDescriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, kItfCount, 0, GPK_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(kItfCdcComm, 4, kEpCdcNotify, 8, kEpCdcOut, kEpCdcIn, 64),
};

const char* const kStrings[] = {
    (const char[]){0x09, 0x04},  // 0: English (US)
    "GekkoPAK",                  // 1: manufacturer
    "DSpico cartridge link",     // 2: product
    "GPKLINK1",                  // 3: serial
    "GekkoPAK telemetry",        // 4: CDC interface
};

u16 sStringBuffer[32];

// --------------------------------------------------------------------------
// Output buffering
// --------------------------------------------------------------------------
//
// The CDC endpoint FIFO holds one packet, and a 512-byte block dump is many
// packets, so responses are queued here and drained as the host reads. A
// response that will not fit is refused rather than truncated: a partial dump
// that looks complete is worse than no dump at all.

constexpr u32 kOutCapacity = 4096;
char sOut[kOutCapacity];
u32 sOutHead;  // next byte to send
u32 sOutTail;  // next free slot
bool sOverflow;

u32 outFree() { return kOutCapacity - 1u - ((sOutTail - sOutHead) % kOutCapacity); }

void putChar(char c) {
    const u32 next = (sOutTail + 1u) % kOutCapacity;
    if (next == sOutHead) {
        sOverflow = true;
        return;
    }
    sOut[sOutTail] = c;
    sOutTail = next;
}

void put(const char* s) {
    while (*s) {
        putChar(*s++);
    }
}

void putHex(u32 value, u32 digits) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (u32 i = digits; i > 0; --i) {
        putChar(kDigits[(value >> ((i - 1u) * 4u)) & 0xFu]);
    }
}

// Hex without leading zeroes, for values whose width carries no meaning.
void putHexShort(u32 value) {
    u32 digits = 8;
    while (digits > 1 && ((value >> ((digits - 1u) * 4u)) & 0xFu) == 0) {
        --digits;
    }
    putHex(value, digits);
}

void putField(const char* key, u32 value) {
    putChar(' ');
    put(key);
    putChar('=');
    putHexShort(value);
}

// --------------------------------------------------------------------------
// Responses
// --------------------------------------------------------------------------

void emitBanner() {
    // Version first, so a harness can refuse to talk to a firmware whose
    // grammar it does not know rather than misparse it.
    put("# gekkopak-link 1 cdc\n");
}

void emitStatus() {
    gekkopak_ntr_state_t state;
    gekkopak_ntr_state(&state);
    put("S");
    putField("resets", state.reset_count);
    putField("f4enter", state.f4_enter);
    putField("f4acc", state.f4_accepted);
    putField("f4comp", state.f4_complete);
    putField("f4parse", state.f4_parsed);
    putField("f5sent", state.f5_sent);
    putField("staged", state.staged_bytes);
    putField("stage", state.stage_index);
    putField("depth", state.event_depth);
    putField("result", state.result);
    putField("local", state.local_bytes);
    putField("mask", gpk_trace_mask());
    putField("tdepth", gpk_trace_depth());
    putField("tdrop", gpk_trace_dropped());
    putChar('\n');
}

// Drains as much of the trace ring as the output buffer can hold, then says how
// much came out and how much the ring had already lost. A drain that stops
// early is not an error: the next `p` continues from where it left off.
void emitTrace() {
    u32 emitted = 0;
    gpk_trace_record_t record;
    // 40 bytes is comfortably more than a formatted record needs.
    while (outFree() > 40u && gpk_trace_pop(&record)) {
        put("T ");
        putHex(record.time_us, 8);
        putChar(' ');
        putHex(record.kind, 2);
        putChar(' ');
        putHex(record.index, 2);
        putChar(' ');
        putHex(record.aux, 4);
        putChar(' ');
        putHex(record.value, 8);
        putChar('\n');
        ++emitted;
    }
    put("P ");
    putHexShort(emitted);
    putChar(' ');
    putHexShort(gpk_trace_dropped());
    putChar('\n');
}

// 32 bytes per line: short enough that a line survives a terminal, long enough
// that a 512-byte block is 16 lines rather than 64.
void emitBlock(u32 which) {
    const u8* data = gekkopak_ntr_buffer(which);
    if (data == nullptr) {
        put("E no such buffer\n");
        return;
    }
    // 16 lines of 71 bytes. Refuse rather than emit a dump with a hole in it.
    if (outFree() < 16u * 72u) {
        put("E busy\n");
        return;
    }
    for (u32 offset = 0; offset < kBlockBytes; offset += 32u) {
        put("X ");
        putHex(offset, 3);
        putChar(' ');
        for (u32 i = 0; i < 32u; ++i) {
            putHex(data[offset + i], 2);
        }
        putChar('\n');
    }
    // Terminator. The grammar carries no length prefix, so without this a
    // reader cannot tell a complete dump from one that is still arriving.
    put("K b ");
    putHexShort(kBlockBytes);
    putChar('\n');
}

// --------------------------------------------------------------------------
// Command parsing
// --------------------------------------------------------------------------

constexpr u32 kLineCapacity = 32;
char sLine[kLineCapacity];
u32 sLineLength;

u32 parseHex(const char* s, u32 length, bool* ok) {
    u32 value = 0;
    u32 digits = 0;
    for (u32 i = 0; i < length; ++i) {
        const char c = s[i];
        u32 nibble;
        if (c >= '0' && c <= '9') {
            nibble = static_cast<u32>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = static_cast<u32>(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            nibble = static_cast<u32>(c - 'A') + 10u;
        } else if (c == ' ') {
            continue;
        } else {
            *ok = false;
            return 0;
        }
        value = (value << 4) | nibble;
        ++digits;
    }
    *ok = digits > 0;
    return value;
}

// Set when the host asks for BOOTSEL. The reboot happens after the reply has
// been flushed, so a harness sees the acknowledgement rather than a port that
// vanished and has to be guessed about.
bool sRebootPending;

// How many task passes to keep trying to flush before rebooting anyway. A host
// that asked to reboot and then stopped reading must not be able to strand the
// cartridge in a state where the only way out is the BOOTSEL button.
constexpr u32 kRebootFlushPasses = 200;
u32 sRebootPasses;

// Hands the cartridge to the bootloader.
//
// reset_usb_boot() is not a chip reset. It calls into the bootrom, which
// reboots by way of the watchdog and then re-enters USB boot on whatever clocks
// it finds. Upstream only ever calls it from tryRebootToBootsel(), which runs
// before pwr_initPowerSaving() and so still has every clock intact -- and even
// there it calls xosc_init() first.
//
// By the time the link can be asked for a reboot, stopUnusedClocks() has taken
// away things the bootrom needs, and the cost of missing any one of them is a
// chip that stops responding with no way back but the BOOTSEL button:
//
//   - clk_sys to the WATCHDOG, gated off in wake_en1. This is the one that
//     matters most: without it the reset the bootrom asks for never arrives and
//     it spins forever, still enumerated, answering nothing.
//   - the TIMER, gated off beside it.
//   - ROSC, disabled outright.
//
// wake_en is restored wholesale rather than bit by bit. Ungating a clock to a
// peripheral nobody is using costs nothing microseconds before a reset, and an
// exact undo of stopUnusedClocks() would be one more thing to get wrong -- the
// first attempt at this restored only ROSC and hung the board twice.
void rebootToBootsel() {
    clocks_hw->wake_en0 = ~0u;
    clocks_hw->wake_en1 = ~0u;

    u32 ctrl = rosc_hw->ctrl;
    ctrl &= ~ROSC_CTRL_ENABLE_BITS;
    ctrl |= ROSC_CTRL_ENABLE_VALUE_ENABLE << ROSC_CTRL_ENABLE_LSB;
    hw_clear_bits(&rosc_hw->status, ROSC_STATUS_BADWRITE_BITS);
    rosc_hw->ctrl = ctrl;

    // initDeepSleep() left SLEEPDEEP set, which would change what a wfi in the
    // bootrom does.
    scb_hw->scr &= ~M0PLUS_SCR_SLEEPDEEP_BITS;

    xosc_init();
    reset_usb_boot(0, 0);
}

void execute(const char* line, u32 length) {
    if (length == 0) {
        return;
    }
    const char command = line[0];
    const char* arg = line + 1;
    const u32 arg_length = length - 1u;

    switch (command) {
        case '?':
            emitBanner();
            break;
        case 's':
            emitStatus();
            break;
        case 'p':
            emitTrace();
            break;
        case 'd':
            emitBlock(GEKKOPAK_BUFFER_STAGED);
            break;
        case 'g':
            emitBlock(GEKKOPAK_BUFFER_DIAG);
            break;
        case 'x':
            emitBlock(GEKKOPAK_BUFFER_LAST_IN);
            break;
        case 't': {
            if (arg_length > 0) {
                bool ok = true;
                const u32 mask = parseHex(arg, arg_length, &ok);
                if (!ok) {
                    put("E bad mask\n");
                    return;
                }
                gpk_trace_set_mask(mask);
            }
            put("K t ");
            putHexShort(gpk_trace_mask());
            putChar('\n');
            break;
        }
        case 'z':
            // Resets the GekkoPAK device only. The cartridge stays enumerated
            // and the card transport is untouched, so a host can clear device
            // state between runs without the console noticing a card reset.
            gekkopak_ntr_reset();
            put("K z\n");
            break;
        case 'B':
            sRebootPending = true;
            sRebootPasses = 0;
            put("K B\n");
            break;
        default:
            put("E unknown command\n");
            break;
    }
}

void feed(char c) {
    if (c == '\n' || c == '\r') {
        if (sLineLength > 0) {
            execute(sLine, sLineLength);
            sLineLength = 0;
        }
        return;
    }
    if (sLineLength < kLineCapacity) {
        sLine[sLineLength++] = c;
    } else {
        // Overlong line. Drop it whole rather than acting on a prefix.
        sLineLength = kLineCapacity;
    }
}

// --------------------------------------------------------------------------
// Pumps
// --------------------------------------------------------------------------

bool sStarted;
bool sAnnounced;

void pumpInput() {
    char buffer[64];
    while (tud_cdc_available()) {
        const u32 read = tud_cdc_read(buffer, sizeof(buffer));
        if (read == 0) {
            break;
        }
        for (u32 i = 0; i < read; ++i) {
            feed(buffer[i]);
        }
    }
}

void pumpOutput() {
    while (sOutHead != sOutTail) {
        const u32 space = tud_cdc_write_available();
        if (space == 0) {
            break;
        }
        // One contiguous run at a time; the wrap is handled by the next pass.
        u32 run = (sOutTail > sOutHead ? sOutTail : kOutCapacity) - sOutHead;
        if (run > space) {
            run = space;
        }
        const u32 written = tud_cdc_write(sOut + sOutHead, run);
        if (written == 0) {
            break;
        }
        sOutHead = (sOutHead + written) % kOutCapacity;
    }
    tud_cdc_write_flush();
}

} // namespace

// --------------------------------------------------------------------------
// tinyusb callbacks
// --------------------------------------------------------------------------

extern "C" const u8* tud_descriptor_device_cb(void) {
    return reinterpret_cast<const u8*>(&kDeviceDescriptor);
}

extern "C" const u8* tud_descriptor_configuration_cb(u8 index) {
    (void)index;
    return kConfigDescriptor;
}

extern "C" const u16* tud_descriptor_string_cb(u8 index, u16 langid) {
    (void)langid;
    u8 count;
    if (index == 0) {
        memcpy(&sStringBuffer[1], kStrings[0], 2);
        count = 1;
    } else {
        if (index >= (sizeof(kStrings) / sizeof(kStrings[0]))) {
            return nullptr;
        }
        const char* s = kStrings[index];
        u32 length = 0;
        while (s[length] != '\0') {
            ++length;
        }
        if (length > 31) {
            length = 31;
        }
        for (u32 i = 0; i < length; ++i) {
            sStringBuffer[1 + i] = static_cast<u16>(s[i]);
        }
        count = static_cast<u8>(length);
    }
    sStringBuffer[0] = static_cast<u16>((TUSB_DESC_STRING << 8) | (2u * count + 2u));
    return sStringBuffer;
}

// --------------------------------------------------------------------------
// Public interface
// --------------------------------------------------------------------------

extern "C" void gekkopak_link_init(void) {
    if (sStarted) {
        return;
    }
    // pwr_initPowerSaving() stops clk_usb and deinitialises pll_usb, and the
    // main loop sleeps deeply; without this the controller has no clock and the
    // port never enumerates. Same call upstream's proxy makes for the same
    // reason.
    pwr_disableUsbPowerSaving();

    // Tracing on by default, in this build only.
    //
    // The mask is normally zero so a timing campaign measures the transport
    // rather than the instrumentation -- but that guarantee belongs to the
    // plain firmware, which has no link and no way to turn tracing on at all.
    // This is the diagnosis build, and requiring a host to arm it means a run
    // started before anyone connected records nothing, which is precisely the
    // run that most needed recording.
    gpk_trace_set_mask(GPK_TRACE_MASK_DEFAULT_ON);

    tud_init(BOARD_TUD_RHPORT);

    // Re-assert the priority upstream chose. The cartridge interrupt runs at
    // 0x40 and must preempt USB, never the other way round: a data phase that
    // is late is a corrupted transfer, whereas a USB packet that is late is
    // just a slower link.
    irq_set_priority(USBCTRL_IRQ, 0x80);

    sStarted = true;
}

extern "C" void gekkopak_link_task(void) {
    if (!sStarted) {
        return;
    }
    tud_task();

    // Before the connection check, not after it. A host that sends `B` closes
    // the port as soon as it sees the acknowledgement -- that is the correct
    // thing for it to do -- and the early return below would then skip the
    // reboot forever, leaving the request pending and the cartridge running.
    if (sRebootPending) {
        // Flush while anyone is still listening, but never wait indefinitely.
        if (!tud_cdc_connected() || sOutHead == sOutTail ||
            ++sRebootPasses >= kRebootFlushPasses) {
            sRebootPending = false;
            rebootToBootsel();
        }
    }

    if (!tud_cdc_connected()) {
        // Nothing is listening. Drop anything queued so that a host connecting
        // later gets the current state rather than a backlog from a run it did
        // not see.
        sOutHead = sOutTail = 0;
        sLineLength = 0;
        sOverflow = false;
        sAnnounced = false;
        return;
    }

    if (!sAnnounced) {
        emitBanner();
        sAnnounced = true;
    }

    pumpInput();
    if (sOverflow) {
        // Say so rather than let a truncated response read as a complete one.
        sOverflow = false;
        put("E output overflow\n");
    }
    pumpOutput();

}
