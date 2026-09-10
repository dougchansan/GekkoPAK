// TinyUSB configuration for the GekkoPAK host link.
//
// The DSpico build already contains a TinyUSB fragment: upstream vendors the
// RP2040 device controller driver, flattened, to serve its E8-EB proxy. This
// configuration belongs to a *different*, complete TinyUSB tree that the
// overlay brings in, and the two are never compiled together -- a build with
// the host link drops upstream's proxy, its event queue and its DCD, so there
// is exactly one device controller driver and exactly one config in the image.
// Mixing them would link two copies of dcd_init(), or worse, compile one tree's
// stack against the other tree's structure layouts.

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU          OPT_MCU_RP2040
// Single-threaded: the stack is pumped from core0's main loop and from the USB
// interrupt, and never from core1, which runs the scrambler generator with all
// interrupts masked.
#define CFG_TUSB_OS           OPT_OS_NONE
#define CFG_TUSB_DEBUG        0

#define BOARD_TUD_RHPORT      0
#define BOARD_TUD_MAX_SPEED   OPT_MODE_FULL_SPEED

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE 64

// CDC only. Every other class costs RAM on a device whose 264 KiB is mostly
// spoken for by the 64 KiB GekkoPAK pool and the card staging buffers.
#define CFG_TUD_CDC           1
#define CFG_TUD_MSC           0
#define CFG_TUD_HID           0
#define CFG_TUD_MIDI          0
#define CFG_TUD_VENDOR        0

// A 512-byte block dump is 16 lines of 71 bytes, so a small FIFO would mean the
// stack is re-entered dozens of times per response. 256 each way keeps a dump
// to a handful of passes without being a meaningful share of RAM.
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
