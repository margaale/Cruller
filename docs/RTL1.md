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
| `get [-o <off>] [-l <len>] -- <path>` | `off len total nonce`          | a file from the SD card, or a piece of it |

Colour byte: bits 5-4 red, 3-2 green, 1-0 blue (2 bits each, ×85 for 8-bit), bits 7-6 background
mode. Text-only queries: `ver`, `model`, `osd2 state`, `banner`.

**get** (tried on 1.87.4): flags `-a` (acknowledged), `-n` (not worked out), `-o <off>`, `-l <len>`
(`get: bad flags (use -a  -n  -o <off>  -l <len>)`). The ready line says which piece comes:
`get ready off=100 len=20 total=42381 nonce=0x5C0D`; `len` stops at the end of the file, and `-l 0`
or no `-l` sends the rest. Refusals: `get err: cannot open <path>` (no such file, or a folder),
`get err: offset past EOF (size=<n>)`, `get: need a path`.

**Line speed:** `baud <rate>` answers `baud switching to <rate> -- send 'baud ok' within 5000 ms`
(or `bad baud <rate> -- use 115200/500000/1000000/2000000`, or `baud: busy`). The host switches its
side, then `baud ok` at the new speed answers `baud confirmed <rate>`; without it the RT4K goes back
by itself. 2 Mbaud is the most it takes.

**Timing:** replies start 1–17 ms after a command. A transfer request sent before the RT4K has
answered the previous console command is ignored (no ready line); right after the answer it's taken.

## Uploads (put)

`put <size> <sha256hex> <path>` writes a file to the SD card; the host sends the frames:

```
> put -a 8 <sha256 of the 8 bytes> notes.txt
< [COM] put ready nonce=0xF109
> DATA seq 0 (8 bytes)            < ACK seq 0
> DATA seq 1 (0 bytes: end of file)   < ACK seq 1
< [COM] put done
```

- DATA payloads up to 2048 bytes, sequence from 0 (wrapping at 256), then an empty DATA frame.
- With `-a` the RT4K ACKs each frame (NAK to resend), ~61 KB/s. Without it the host streams and the
  RT4K paces it with CTS, which it wires to the FT232R: switch RTS/CTS on at the chip first (FTDI
  `SET_FLOW_CTRL`, wIndex `0x0100` for the FT232R). ~94 KB/s, the RT4K's own pace (CTS drops while
  it writes the card). Cruller keeps flow control on all the time and streams.
- The RT4K writes to `.rtl1up.tmp`, checks size and SHA-256, then renames: a mismatch answers
  `put fail: size/sha mismatch` and leaves nothing. Existing files are replaced.
- It doesn't create folders (`put err: rename failed`, the temp file stays): `mkdir` first.
- Paths are the rest of the line, relative to the SD root; spaces and brackets are fine.
- No frames for about 6 s: a NAK (reason 6) and `put timeout`.
- Other replies: `put: usage ...`, `put err: ...`.

## Files and firmware

| Command                  | Reply                                                      |
|--------------------------|------------------------------------------------------------|
| `ls [dir]`               | `ent t=F\|D sz=<bytes> mt=<unix time> nm=<name>` per entry, then `ls end <count>`; `ls err=2 NOSUCH` |
| `stat <path>`            | `stat t=F\|D sz=... mt=... at=0x20\|0x10 nm=<path>`, or `stat err=2 NOSUCH` |
| `mkdir <path>`           | `mkdir ok`, or `mkdir err=64 EXIST`                        |
| `rm <path>`              | `rm ok` (empty folders too), `rm err=70 NOTEMPTY`, `rm err=2 NOSUCH` |
| `mv <old>\|<new>`        | `mv ok` (across folders too), `mv err=2 NOSUCH`, `mv: usage 'mv <old>\|<new>'` |
| `fwup check`             | `fwup ok version=<v> token=<hex>` once `rt4kup.bin` and its `.rbf` are on the card |
| `fwup go <token>`        | `fwup: flashing`; the RT4K restarts and installs (~40 s)   |

Paths are the rest of the line, with or without a leading `/`; spaces are fine. `ls` takes no flags
(`ls -h` lists a folder called `-h`) and sends the whole folder at once, about 0.7 ms a line
(55 entries in 38 ms). None of these answer while the RT4K is in standby. Files written over RTL1
get the time stamp 1577836800 (2020-01-01): the RT4K has no clock.

A firmware zip from RetroTINK holds `rt4kup.bin`, one `.rbf` per model (`rt4k_` Pro, `rt4kce_` CE,
`rt6x_` 6X) and sometimes extra folders (`lumacode/`). Cruller's page writes all of it except the
other models' `.rbf`, `rt4kup.bin` last, then runs `fwup` (tried: 1.87.0 to 1.87.3).

## Cruller

`src/core/rtl1.c` sees every byte from the RT4K. Text goes to the terminal; between a ready line and the
end of the transfer, bytes go to the frame decoder instead, so the terminal never shows binary.
Frames are checked for CRC, nonce and sequence, and the payload against the SHA-256 (the RP2350's
hardware engine). One transfer runs at a time, and terminal commands wait for it.
`GET /rt4k/xfer?cmd=osd|osd2|font` returns a verified payload, with the ready line in `X-Ready`.
`POST /rt4k/put?path=<path>&sha=<sha256>` writes the request body to the SD card;
`POST /rt4k/ask?expect=<text>` sends the body as a command and returns the first reply line
containing `<text>`.
`GET /rt4k/ls?dir=<path>` lists a folder (`D|F`, size, time and name, tab-separated, one entry a
line; `X-Total` has the RT4K's count), and `GET /rt4k/get?path=<path>` downloads a file in 16 KB
`get -o/-l` pieces, each verified before it goes out.
