// TinyUSB: native USB port as host, for the RT4K's USB-C serial port (an FTDI FT232R).
// Based on the TinyUSB host/cdc_msc_hid example configuration (MIT).

#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                   OPT_OS_PICO
#endif
#define CFG_TUSB_DEBUG                0
#define CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_ALIGN             __attribute__((aligned(4)))

#define CFG_TUH_ENABLED               1
#define BOARD_TUH_RHPORT              0
#define CFG_TUH_MAX_SPEED             OPT_MODE_DEFAULT_SPEED

#define CFG_TUH_ENUMERATION_BUFSIZE   256
#define CFG_TUH_HUB                   0
#define CFG_TUH_DEVICE_MAX            1

// Serial adapters: CDC ACM plus the vendor bridges TinyUSB exposes through the CDC API.
#define CFG_TUH_CDC                   1
#define CFG_TUH_CDC_FTDI              1
#define CFG_TUH_CDC_CP210X            1
#define CFG_TUH_CDC_CH34X             1
// TinyUSB before 0.21 declared rx_ff_buf with CFG_TUH_CDC_TX_BUFSIZE: with a bigger RX size the FIFO
// wrote past its buffer during RTL1 bursts. 0.21 is fixed; keeping them equal costs nothing.
#define CFG_TUH_CDC_RX_BUFSIZE        2048  // ~10 ms at 2 Mbaud, slack for the rt4k task
#define CFG_TUH_CDC_TX_BUFSIZE        CFG_TUH_CDC_RX_BUFSIZE
// Several packets per RX transfer: the host driver chains them in its IRQ instead of waiting for the
// rt4k task to re-arm after each 64-byte packet. Needs TinyUSB to strip the FTDI's 2 status bytes
// per packet (src/platform/rp2/patches/tinyusb/0001-cdc-host-ftdi-strip-status-per-packet.patch).
#define CFG_TUH_CDC_RX_EPSIZE         512
// Same for TX (default: one 64-byte packet per transfer, a task round trip each). Fewer round trips;
// uploads still top out at ~109 KB/s on the line because the RT4K holds CTS while it writes its card.
#define CFG_TUH_CDC_TX_EPSIZE         512

// On enumeration: DTR+RTS on, and 2 Mbaud 8N1 (the RT4K's USB serial rate).
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM  0x03
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM   { 2000000, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

#endif // TUSB_CONFIG_H
