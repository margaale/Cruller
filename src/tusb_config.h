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
#define CFG_TUH_CDC_RX_BUFSIZE        512
#define CFG_TUH_CDC_TX_BUFSIZE        512

// On enumeration: DTR+RTS on, and 2 Mbaud 8N1 (the RT4K's USB serial rate).
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM  0x03
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM   { 2000000, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

#endif // TUSB_CONFIG_H
