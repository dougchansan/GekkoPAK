// tinyusb device controller driver for the DSpico, driven from the ARM9.
//
// The DSpico exposes the RP2040's USB device controller through card commands:
// E8 control, E9 write endpoint buffer, EA read endpoint buffer, EB dequeue
// event. Anything that owns the card bus can therefore drive USB, and this
// application already does.
//
// Ported from LNH-team/dspico-usb-examples platform/dcd_dspico.cpp, with three
// deliberate differences:
//
//   * No ARM7 and no threads. The reference runs USB on an ARM7 thread woken by
//     the card IRQ, because its ARM9 is busy serving disk sectors. Here the ARM9
//     owns the bus outright, so the event pump is gpk_usb_task(), called from
//     the main loop. That also removes the card mutex the reference needs.
//
//   * No DMA. The reference uses DMA3 for the 512-byte endpoint buffers; CPU
//     copies are used here to match the rest of this application's transport and
//     keep the bus access in one style.
//
//   * Events are polled rather than interrupt-driven, which suits a design that
//     services USB between test stages instead of concurrently with them.
//
// The last point matters for measurements: USB traffic is card traffic, so it
// must not overlap a timed region. gpk_usb_task() is only called outside them.

#include <nds.h>
#include "tusb_option.h"

#if CFG_TUD_ENABLED

#include "device/dcd.h"

// Card bus registers, as in gekkopak_ntr.c.
#define REG_MCCNT0 (*(vu16 *)0x040001A0)
#define REG_MCCNT1 (*(vu32 *)0x040001A4)
#define REG_MCCMD0 (*(vu32 *)0x040001A8)
#define REG_MCD1   (*(vu32 *)0x04100010)

#define MCCNT0_MODE_MASK             (1 << 13)
#define MCCNT0_MODE_ROM              (0 << 13)
#define MCCNT0_ENABLE                (1 << 15)
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

#define USB_BASE_SETTINGS (MCCNT1_RESET_OFF | MCCNT1_CLK_6_7_MHZ | MCCNT1_CMD_SCRAMBLE | \
                           MCCNT1_CLOCK_SCRAMBLER | MCCNT1_READ_DATA_DESCRAMBLE)

// Command encodings, from the DSpico USB platform headers.
#define USB_CMD(c, args)          (0xE800000000000000ull | ((uint64_t)(c) << 48) | (args))
#define USB_CMD_INIT              USB_CMD(1, 0)
#define USB_CMD_BEGIN_SET_ADDR    USB_CMD(2, 0)
#define USB_CMD_REMOTE_WAKEUP     USB_CMD(3, 0)
#define USB_CMD_CONNECT           USB_CMD(4, 0)
#define USB_CMD_DISCONNECT        USB_CMD(5, 0)
#define USB_CMD_SOF_ENABLE        USB_CMD(6, 0)
#define USB_CMD_SOF_DISABLE       USB_CMD(7, 0)
#define USB_CMD_EP_CLOSE_ALL      USB_CMD(8, 0)
#define USB_CMD_EP_STALL(ep)      USB_CMD(9,  (uint64_t)(ep) << 40)
#define USB_CMD_EP_CLR_STALL(ep)  USB_CMD(10, (uint64_t)(ep) << 40)
#define USB_CMD_EP_CLOSE(ep)      USB_CMD(11, (uint64_t)(ep) << 40)
#define USB_CMD_FINISH_SET_ADDR(a) USB_CMD(12, (uint64_t)(a) << 40)
#define USB_CMD_EP_OPEN(ep, mps, xfer) \
    USB_CMD(13, ((uint64_t)(ep) << 40) | ((uint64_t)((xfer) & 3) << 32) | ((uint64_t)((mps) & 0x7FF)))
#define USB_CMD_DEINIT            USB_CMD(15, 0)
#define USB_CMD_BEGIN_XFER(ep, off, len) \
    USB_CMD(16, ((uint64_t)(ep) << 40) | ((uint64_t)(off) << 32) | (uint64_t)(len))
#define USB_CMD_INT_ENABLE        USB_CMD(17, 0)
#define USB_CMD_INT_DISABLE       USB_CMD(18, 0)

#define USB_WRITE_DATA(off, ep, last, total) \
    (0xE900000000000000ull | ((uint64_t)(off) << 48) | ((uint64_t)(ep) << 40) | \
     ((uint64_t)(last) << 32) | (uint64_t)(total))
#define USB_READ_DATA(off, ep) \
    (0xEA00000000000000ull | ((uint64_t)(off) << 32) | ((uint64_t)(ep) << 40))
#define USB_GET_EVENT             0xEB00000000000000ull

static inline void bus_start(uint32_t settings)
{
    REG_MCCNT0 = (REG_MCCNT0 & ~MCCNT0_MODE_MASK) | MCCNT0_MODE_ROM | MCCNT0_ENABLE;
    REG_MCCNT1 = MCCNT1_ENABLE | settings;
}

static inline void bus_set_cmd(uint64_t cmd)
{
    *(vu64 *)&REG_MCCMD0 = __builtin_bswap64(cmd);
}

// Bounded, like the rest of this application's bus access: a cartridge that
// fails to finish a transfer must not lock the app up with no output.
static inline void bus_wait_idle(void)
{
    uint32_t guard = 0;
    while ((REG_MCCNT1 & MCCNT1_ENABLE) && ++guard < 400000) { }
}

static void usb_cmd(uint64_t command)
{
    bus_set_cmd(command);
    bus_start(USB_BASE_SETTINGS | MCCNT1_DIR_READ | MCCNT1_LEN_0 | MCCNT1_LATENCY2(0));
    bus_wait_idle();
}

static uint32_t usb_cmd_read32(uint64_t command)
{
    uint32_t value = 0, guard = 0;
    bus_set_cmd(command);
    bus_start(USB_BASE_SETTINGS | MCCNT1_DIR_READ | MCCNT1_LEN_4 | MCCNT1_LATENCY2(4));
    do {
        if (REG_MCCNT1 & MCCNT1_DATA_READY)
            value = REG_MCD1;
    } while ((REG_MCCNT1 & MCCNT1_ENABLE) && ++guard < 400000);
    return value;
}

static void usb_write_block(uint64_t command, const uint8_t *src, uint32_t len)
{
    uint32_t sent = 0, guard = 0, word = 0;
    bus_set_cmd(command);
    bus_start(USB_BASE_SETTINGS | MCCNT1_DIR_WRITE | MCCNT1_LEN_512 | MCCNT1_LATENCY2(8));
    do {
        if (REG_MCCNT1 & MCCNT1_DATA_READY) {
            if (sent + 4 <= len) {
                word = (uint32_t)src[sent] | ((uint32_t)src[sent + 1] << 8) |
                       ((uint32_t)src[sent + 2] << 16) | ((uint32_t)src[sent + 3] << 24);
                sent += 4;
            } else if (sent < len) {
                word = 0;
                for (uint32_t i = 0; sent < len; i++, sent++)
                    word |= (uint32_t)src[sent] << (i * 8);
            } else {
                word = 0;
            }
            REG_MCD1 = word;
        }
    } while ((REG_MCCNT1 & MCCNT1_ENABLE) && ++guard < 400000);
}

static void usb_read_block(uint64_t command, uint8_t *dst, uint32_t len)
{
    uint32_t got = 0, guard = 0;
    bus_set_cmd(command);
    bus_start(USB_BASE_SETTINGS | MCCNT1_DIR_READ | MCCNT1_LEN_512 | MCCNT1_LATENCY2(4));
    do {
        if (REG_MCCNT1 & MCCNT1_DATA_READY) {
            uint32_t w = REG_MCD1;
            for (uint32_t i = 0; i < 4 && got < len; i++, got++)
                dst[got] = (uint8_t)(w >> (i * 8));
        }
    } while ((REG_MCCNT1 & MCCNT1_ENABLE) && ++guard < 400000);
}

// ---------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------
typedef struct {
    const uint8_t *tx;
    uint8_t       *rx;
    uint32_t       length;
    uint32_t       offset;
    uint32_t       remaining;
    uint32_t       received;
    uint8_t        addr;
    uint8_t        buf;      // which of DSpico's two 512-byte buffers
    bool           active;
} usb_ep_t;

static usb_ep_t sIn[16];
static usb_ep_t sOut[16];

static void in_send_block(usb_ep_t *ep, bool start)
{
    uint32_t len = ep->length - ep->offset;
    if (len > 512)
        len = 512;

    if (len > 0) {
        usb_write_block(USB_WRITE_DATA(ep->buf, ep->addr, start ? 1 : 0, len),
                        ep->tx + ep->offset, len);
        ep->offset += len;
        ep->buf = 1 - ep->buf;
    } else if (start) {
        // Zero-length packet: no data phase.
        usb_cmd(USB_WRITE_DATA(ep->buf, ep->addr, 1, 0));
    }
}

static void in_complete(usb_ep_t *ep, uint32_t transferred)
{
    if (!ep->active) {
        dcd_event_xfer_complete(0, ep->addr, transferred, XFER_RESULT_SUCCESS, false);
        return;
    }
    ep->remaining -= transferred;
    if (ep->remaining == 0) {
        dcd_event_xfer_complete(0, ep->addr, ep->length, XFER_RESULT_SUCCESS, false);
        ep->active = false;
        return;
    }
    uint32_t len = ep->remaining > 512 ? 512 : ep->remaining;
    usb_cmd(USB_CMD_BEGIN_XFER(ep->addr, 1 - ep->buf, len));
    in_send_block(ep, false);
}

static void out_receive_block(usb_ep_t *ep)
{
    uint32_t len = ep->received - ep->offset;
    if (len == 0)
        return;
    if (len > 512)
        len = 512;
    usb_read_block(USB_READ_DATA(ep->buf, ep->addr), ep->rx + ep->offset, len);
    ep->offset += len;
    ep->buf = 1 - ep->buf;
}

static void out_complete(usb_ep_t *ep, uint32_t transferred)
{
    if (!ep->active) {
        dcd_event_xfer_complete(0, ep->addr, transferred, XFER_RESULT_SUCCESS, false);
        return;
    }
    ep->received += transferred;
    int32_t len = (int32_t)ep->length - (int32_t)ep->offset - 512;
    if (len > 512)
        len = 512;
    if (transferred == 512 && len > 0)
        usb_cmd(USB_CMD_BEGIN_XFER(ep->addr, 1 - ep->buf, (uint32_t)len));

    out_receive_block(ep);

    if (transferred < 512 || len == 0) {
        dcd_event_xfer_complete(0, ep->addr, ep->offset, XFER_RESULT_SUCCESS, false);
        ep->active = false;
    }
}

// ---------------------------------------------------------------------------
// Event pump. Called from the main loop instead of an IRQ-woken thread.
// ---------------------------------------------------------------------------
void gpk_usb_task(void)
{
    for (int guard = 0; guard < 64; guard++) {
        uint32_t event = usb_cmd_read32(USB_GET_EVENT);
        bool last = (event >> 31) != 0;
        event &= ~(1u << 31);

        if (event == 0)
            return;  // USB_EVENT_NONE

        if ((event >> 30) == 1) {
            // SETUP packet arrives as two consecutive events.
            tusb_control_request_t setup;
            setup.wLength = (event >> 16) & 0x1FFF;
            uint32_t direction = (event >> 29) & 1;
            setup.wIndex = event & 0xFFFF;

            event = usb_cmd_read32(USB_GET_EVENT);
            last = (event >> 31) != 0;
            event &= ~(1u << 31);
            setup.wValue = event & 0xFFFF;
            setup.bRequest = (event >> 16) & 0xFF;
            setup.bmRequestType = (event >> 24) & 0x7F;
            setup.bmRequestType_bit.direction = direction;
            dcd_event_setup_received(0, (const uint8_t *)&setup, false);
        } else if ((event >> 28) == 2) {
            dcd_event_sof(0, event & 0x7FF, false);
        } else if ((event >> 28) == 3) {
            uint32_t ep = event & 0xFF;
            uint32_t transferred = (event >> 8) & 0x1FFF;
            if (tu_edpt_dir(ep) == TUSB_DIR_IN)
                in_complete(&sIn[ep & 0x0F], transferred);
            else
                out_complete(&sOut[ep & 0x0F], transferred);
        } else if (event == 1) {
            dcd_event_bus_reset(0, TUSB_SPEED_FULL, false);
        } else if (event == 2) {
            dcd_event_bus_signal(0, DCD_EVENT_UNPLUGGED, false);
        } else if (event == 3) {
            dcd_event_bus_signal(0, DCD_EVENT_SUSPEND, false);
        } else if (event == 4) {
            dcd_event_bus_signal(0, DCD_EVENT_RESUME, false);
        }

        if (last)
            return;
    }
}

// ---------------------------------------------------------------------------
// Device API
// ---------------------------------------------------------------------------
bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    (void)rhport; (void)rh_init;
    for (int i = 0; i < 16; i++) {
        sIn[i].addr = (uint8_t)(TUSB_DIR_IN_MASK | i);
        sOut[i].addr = (uint8_t)i;
        sIn[i].active = sOut[i].active = false;
    }
    usb_cmd(USB_CMD_INIT);
    return true;
}

bool dcd_deinit(uint8_t rhport)          { (void)rhport; usb_cmd(USB_CMD_DEINIT); return true; }
// Interrupts stay disabled: events are polled by gpk_usb_task().
void dcd_int_enable(uint8_t rhport)      { (void)rhport; }
void dcd_int_disable(uint8_t rhport)     { (void)rhport; }
void dcd_set_address(uint8_t rhport, uint8_t a) { (void)rhport; (void)a; usb_cmd(USB_CMD_BEGIN_SET_ADDR); }
void dcd_remote_wakeup(uint8_t rhport)   { (void)rhport; usb_cmd(USB_CMD_REMOTE_WAKEUP); }
void dcd_connect(uint8_t rhport)         { (void)rhport; usb_cmd(USB_CMD_CONNECT); }
void dcd_disconnect(uint8_t rhport)      { (void)rhport; usb_cmd(USB_CMD_DISCONNECT); }
void dcd_sof_enable(uint8_t rhport, bool en)
{
    (void)rhport;
    usb_cmd(en ? USB_CMD_SOF_ENABLE : USB_CMD_SOF_DISABLE);
}

void dcd_edpt0_status_complete(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
        request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        request->bRequest == TUSB_REQ_SET_ADDRESS) {
        usb_cmd(USB_CMD_FINISH_SET_ADDR((uint8_t)request->wValue));
    }
}

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *desc)
{
    (void)rhport;
    usb_cmd(USB_CMD_EP_OPEN(desc->bEndpointAddress, desc->wMaxPacketSize,
                            desc->bmAttributes.xfer));
    return true;
}

void dcd_edpt_close_all(uint8_t rhport)  { (void)rhport; usb_cmd(USB_CMD_EP_CLOSE_ALL); }
void dcd_edpt_close(uint8_t rhport, uint8_t ep) { (void)rhport; usb_cmd(USB_CMD_EP_CLOSE(ep)); }
void dcd_edpt_stall(uint8_t rhport, uint8_t ep) { (void)rhport; usb_cmd(USB_CMD_EP_STALL(ep)); }
void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep) { (void)rhport; usb_cmd(USB_CMD_EP_CLR_STALL(ep)); }

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes)
{
    (void)rhport;
    if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN) {
        usb_ep_t *ep = &sIn[ep_addr & 0x0F];
        ep->tx = buffer;
        ep->length = ep->remaining = total_bytes;
        ep->offset = 0;
        ep->buf = 0;
        ep->active = true;
        in_send_block(ep, true);
        in_send_block(ep, false);
    } else {
        usb_ep_t *ep = &sOut[ep_addr & 0x0F];
        ep->rx = buffer;
        ep->length = total_bytes;
        ep->offset = ep->received = 0;
        ep->buf = 0;
        ep->active = true;
        uint32_t first = total_bytes > 512 ? 512 : total_bytes;
        usb_cmd(USB_CMD_BEGIN_XFER(ep->addr, ep->buf, first));
    }
    return true;
}

#endif
