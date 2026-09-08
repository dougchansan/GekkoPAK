# USB CDC results channel

Streams the test transcript to the host as a serial port, so a run's results do
not have to be photographed off the screen or written to the SD card.

## Why this exists

Collecting results has been the slowest part of hardware bring-up. Photographs
are reliable but manual and error-prone; writing to the SD card is not usable at
all, because this DLDI path stops accepting writes once GekkoPAK bus traffic has
started, and the failure is silent.

## How it works

The DSpico exposes the RP2040's USB device controller through card commands -
`E8` control, `E9` write endpoint buffer, `EA` read endpoint buffer, `EB`
dequeue event. Anything owning the card bus can drive USB, and this application
already does, so USB needs no extra hardware path and no second CPU.

`dcd_dspico.c` is a tinyusb device controller driver built on those commands,
ported from `LNH-team/dspico-usb-examples`. Three deliberate differences:

- **No ARM7 and no threads.** The reference runs USB on an ARM7 thread woken by
  the card IRQ, because its ARM9 is busy serving disk sectors over IPC. Here the
  ARM9 owns the bus outright, so the event pump is `gpk_usb_task()` called from
  the main loop, and the reference's card mutex disappears with the threads.
- **No DMA**, matching the CPU-copy style of the rest of the transport.
- **Polled events**, which suits servicing USB between test stages rather than
  concurrently with them.

That last point is a correctness requirement, not a preference: USB traffic *is*
card traffic, so it must never overlap a timed region or it would corrupt the
measurement. `gpk_usb_task()` is only ever called outside them.

Because tinyusb sees `CFG_TUSB_MCU = OPT_MCU_NONE` it compiles no built-in
device driver, and `CFG_TUSB_OS = OPT_OS_NONE` gives the single-threaded model
this design wants - so the custom OS layer the reference needs is not required.

## Host side

CDC ACM was chosen over a vendor class so Windows binds `usbser.sys`
automatically: the DSpico appears as a COM port with no driver install.
