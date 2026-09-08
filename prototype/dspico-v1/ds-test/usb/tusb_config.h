// tinyusb configuration for the GekkoPAK DS test application.
//
// Single-threaded: OPT_OS_NONE, with tud_task() pumped from the main loop. The
// DSpico USB examples use a custom OS layer only because they run USB on a
// separate ARM7 thread; this application owns the card bus on the ARM9 and can
// service USB between test stages instead.
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// No built-in DCD matches: the device controller lives across the cartridge bus
// and is driven by card commands, so dcd_dspico.c below provides the port.
#define CFG_TUSB_MCU              OPT_MCU_NONE
#define CFG_TUSB_OS               OPT_OS_NONE
#define CFG_TUSB_DEBUG            0
#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED
#define TUP_DCD_ENDPOINT_MAX      16

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#define CFG_TUD_ENDPOINT0_SIZE    64

// Serial only: the point of this is to stream the transcript to the host.
#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_AUDIO             0
#define CFG_TUD_VENDOR            0

#define CFG_TUD_CDC_RX_BUFSIZE    256
#define CFG_TUD_CDC_TX_BUFSIZE    512
#define CFG_TUD_CDC_EP_BUFSIZE    64

#endif
