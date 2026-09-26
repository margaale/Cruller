# RTL1: the RT4K's binary transfer protocol

RetroTINK has not published documentation for the serial interface added in firmware 1.75 yet.
This page describes what Cruller implements, as observed on an RT4K Pro running firmware 1.87.0
over the USB-C serial port (FTDI FT232R, 2 Mbaud 8N1) and cross-checked against the behaviour of
public client software. Details may change with RT4K firmware updates.

## Text and binary on one line

The serial line carries the RT4K's usual text console. Commands are sent as `\r<command>\r\n`;
replies are lines ending in `\n`, prefixed with `[COM] `.

A binary transfer starts with an ordinary text command. The RT4K answers with a *ready* line that
carries a 16-bit session nonce, sends binary frames, then closes with another text line:

```
> osd2
< [COM] osd2 ready on=1 osk=0 rows=4 cols=32 stride=64 cells=2048 nonce=0xFD32
< DATA seq 0 (2048 bytes)   DATA seq 1 (2048 bytes)   RESPONSE seq 2 (SHA-256)
< [COM] osd done
```

Refusals come back as text instead of a ready line (`busy`, `bad command`, `unknown command`,
`nothing shown`, ...).

## Frame format

All multi-byte fields are little-endian.

| Offset | Size | Field                                                        |
|-------:|-----:|--------------------------------------------------------------|
| 0      | 2    | Magic `A5 5A`                                                |
| 2      | 2    | Nonce, from the ready line                                   |
| 4      | 2    | Payload length, at most 2048                                 |
| 6      | 1    | Type                                                         |
| 7      | 1    | Sequence number (starts at 0, wraps at 256)                  |
| 8      | n    | Payload                                                      |
| 8 + n  | 2    | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over bytes 2 .. 8+n-1 |

Types: 1 command, 2 response, 3 data, 4 acknowledgement, 5 negative acknowledgement (payload[0]:
1 CRC, 2 nonce, 3 sequence, 4 length, 5 busy, 6 state), 6 abort, 7 ping.

## A transfer

- DATA frames carry the payload in order; their sequence numbers count up from 0.
- The RESPONSE frame carries the SHA-256 of all the DATA payload bytes (32 bytes) and uses the next
  sequence number.
- The closing text line ends in `done` (`osd done`, `get done`), or reports `aborted` / `failed`.
- To stop a transfer early, the host sends an ABORT frame with the session nonce and the sequence
  number it expected next, then discards input until the closing line.
- Over USB the RT4K streams without waiting. Adding `-a` to the command (`osd -a`) makes it wait
  for an ACK frame (same nonce and sequence) after each frame; clients use that on the slower HD-15
  UART. Cruller doesn't need it.

## Commands used so far

| Command          | Ready line fields                                   | Payload                          |
|------------------|-----------------------------------------------------|----------------------------------|
| `osd`            | `rows stride width cells nonce`                     | main OSD plane: 2048 character codes, then 2048 colour bytes |
| `osd2`           | `on osk rows cols stride cells nonce`               | secondary plane (messages, on-screen keyboard), same layout |
| `font`           | `size glyphs w h layout nonce`                      | 256 glyphs, 8×16, row-major, 4096 bytes |
| `get -- <path>`  | `off len total nonce`                               | a file from the SD card, in chunks |

Colour byte: bits 5-4 red, 3-2 green, 1-0 blue (2 bits each, ×85 for 8-bit), bits 7-6 background
mode. Text-only queries: `ver`, `osd2 state`, `banner`, and `baud <rate>` / `baud ok` to change
the line speed.

## Cruller

`src/rtl1.c` sees every byte from the RT4K. Text goes to the terminal; between a ready line and the
end of the transfer, bytes go to the frame decoder instead, so the terminal never shows binary.
Frames are checked for CRC, nonce and sequence, and the payload against the SHA-256 (the RP2350's
hardware engine). One transfer runs at a time, and terminal commands wait for it.
`GET /rt4k/xfer?cmd=osd|osd2|font` returns a verified payload, with the ready line in `X-Ready`.
