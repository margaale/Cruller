# Cruller design

Cruller is firmware for the Raspberry Pi Pico 2 W that controls a RetroTINK 4K (RT4K) over Wi-Fi. It switches RT4K profiles from the gameID that consoles report, and provides a web UI to control the RT4K. It takes the idea of [DonutShop](https://github.com/svirant/DonutShop) and is written from scratch on the Pico SDK, without Arduino.

Status: M1 (RT4K link) done, parts of M3 and M4 done; M2 (gameID) not started. Details under "Milestones".

## Goals

- **Over-the-air updates from the first image.** The board sits behind the RT4K and should never need a USB cable after the first install.
- **Installable by OTA from the DonutShop Pico 2 W firmware** (the arduino-pico port in margaale/DonutShop), keeping the user's Wi-Fi credentials and gameID configuration.
- **Robust networking.** Use the Pico SDK's own FreeRTOS + lwIP + CYW43 integration, with lwIP core locking. The arduino-pico FreeRTOS layers showed races, a skipped CYW43 mutex and 8–15 s CYW43 bring-ups (see "Lessons from the DonutShop port").
- **A two-way RT4K link over USB** (the RT4K is an FTDI FT232R, 0403:6001, at 2 Mbaud) and over the HD-15 serial port.

Non-goals for now: other scalers, and reusing the DonutShop v0.7 web UI (its source isn't published).

## Hardware

Raspberry Pi Pico 2 W (RP2350, 520 KB RAM, 4 MB flash, CYW43439). The pinout is the same as the DonutShop port, so existing boards keep working:

| Function | Pins | Notes |
| --- | --- | --- |
| RT4K USB | native micro-USB, USB host | micro-USB OTG adapter that also powers the bus |
| RT4K HD-15 serial | UART1 TX GP8, RX GP9 | pull-ups to 3.3 V (not used yet) |
| Extron sw1 | UART0 TX GP0, RX GP1 | not used yet |
| Extron sw2 | PIO UART TX GP4, RX GP5 | not used yet |
| IR receiver / emitter | GP2 / GP3 | optional, not used yet |
| RGB status LED | GP16 / GP17 / GP18 | optional, active low |
| Status LED | CYW43 GPIO 0 | on-board |

## Source layout

Cruller is being split so it can run on other boards (next: the ESP32-S3 on ESP-IDF).

- `src/core`: the common code (HTTP, WebSocket, console, RTL1, RFC 2217, power, SVS, settings) and the interfaces each board implements: `rt4k.h`, `net.h`, `ota.h`, `store.h`, `health.h`, `freeze.h`, `log.h`, `status_led.h`. It uses only FreeRTOS, lwIP's sockets and `src/platform/platform.h` (time, short locks, SHA-256, board id, reboot, memory figures).
- `src/platform/<target>`: a board's side, with its own build (`src/platform/rp2/CMakeLists.txt`; `scripts/build.sh rp2` into `build/rp2`). `rp2` is the Raspberry Pi Pico 2 W on the Pico SDK: startup, CYW43 Wi-Fi and the setup portal, the RT4K's USB host, flash (A/B OTA, `store.h` records, the DonutShop migration), watchdog and freeze recorder.
- `src/web`: the page and `embed.cmake`, which turns it into C arrays at build time.
- `third_party/littlefs`: reads DonutShop's filesystem once, when migrating (rp2).

## Software stack

- Pico SDK 2.3, built with CMake (`scripts/build.sh`). Arm GNU toolchain 14.2.
- FreeRTOS-Kernel SMP (the SDK's RP2350 port).
- lwIP with `pico_cyw43_arch_lwip_sys_freertos`: lwIP runs in its own tcpip thread, and application code takes the lwIP core lock. That lock is the only way into lwIP.
- The CYW43 driver through `pico_cyw43_arch`, initialized from core 0 so its async context and IRQ stay there.
- TinyUSB 0.21 in host mode on the native port (not the SDK's 0.18: its host runs bulk transfers once per frame, too slow for 2 Mbaud). Two patches in `src/platform/rp2/patches/tinyusb`: FTDI status bytes stripped per packet (so transfers can span several packets) and a callback with each packet's status bytes.
- littlefs for configuration and lwIP's mDNS responder.

## Tasks

| Task | Priority | Core | Role |
| --- | --- | --- | --- |
| rt4k | 6 | 1 | the only task touching TinyUSB: host stack, RX into the RTL1 engine, TX queue |
| main | 4 | any | startup; confirms a trial (TBYB) image once healthy |
| async_context_t | 4 | 0 | CYW43 driver (SDK) |
| console | 3 | any | the RT4K console queue: one command at a time, reply windows (see "RT4K console") |
| net | 3 | any | Wi-Fi: join, fallback to the provisioning portal, mDNS |
| http | 2 | any | HTTP server (page, API, OTA upload), hands WebSockets over to ws |
| ws | 2 | any | WebSocket clients: terminal, mirror planes, log, status |
| mirror | 2 | any | polls the RT4K's OSD planes for the page's screen mirror |
| rfc2217 | 2 | any | RFC 2217 server on port 2217 |
| power | 2 | any | RT4K power state probes |
| wdt, ping | 2 | any | watchdog feeder; gateway pings (no traffic for 10 s: let the watchdog reset) |
| led | 1 | any | status LED |
| tcpip_thread | 1 | any | lwIP (SDK) |

The RT4K receive path (TinyUSB, CDC, the RTL1 engine, the FreeRTOS kernel) runs from RAM: from flash it went cold in the XIP cache while Wi-Fi code ran on core 0, and the FT232R overflowed (`src/platform/rp2/cmake/linker/default_text_excludes.incl`).

## RT4K link

- **USB:** the RT4K enumerates as an FTDI FT232R. 2 Mbaud is the most it takes (`baud` accepts 115200, 500000, 1000000 and 2000000). Hot-plug works: the link is up when the device mounts.
- **RTL1:** the binary transfer protocol of firmware 1.75+ (OSD planes, font, files, uploads), in `src/core/rtl1_core.c` (pure, host-tested) and `src/core/rtl1.c`. See [RTL1.md](RTL1.md), which also covers the file and firmware commands.
- **Flow control:** the RT4K wires CTS to the FT232R. Cruller keeps RTS/CTS on at the chip all the time (asked again at every mount), so uploads stream (~94 KB/s, the RT4K's own pace while it writes its card) and commands wait instead of being lost while the RT4K is busy. The RT4K keeps CTS asserted in standby too: "pwr on" gets through with flow control on (measured).
- **HD-15:** not used yet.

### RT4K console

The RT4K's text console is shared by the web page, Cruller's own checks, `/api/command` and RFC 2217 clients. Its replies carry no sender, so `src/core/console.c` sends one command at a time and opens a reply window: lines that come back meanwhile belong to that command's sender. The window closes on the reply's known last line (`Serial Remote:` for a key, `Build tag:` for `ver`, `ls end`...; a `Bad Command` always ends it), otherwise after 50 ms of quiet, or 1 s after a command nothing answered. Lines outside any window go to everyone. Windows last the RT4K's own reply time, 4–23 ms.

An OSD transfer waits until the RT4K has answered the last command (it ignores transfer requests sent right behind one), at most 100 ms. Key to screen on the page: ~91 ms.

### Power state

`src/core/power_core.c` (pure, host-tested) follows what the RT4K shows: any `[COM]` reply means on, `Power On Requested` means starting, `Serial Remote: pwr` or unanswered polls and probes mean standby. It probes with `ver`, which a sleeping RT4K ignores without waking: every 5 s in standby, every second while starting, and after 10 s without a sign of life when on. The mirror stops polling while the RT4K sleeps. (The line break an RT4K is said to produce when it powers down never showed up over USB.)

## Interfaces

- **Web page** (`/`): screen mirror of the RT4K's OSD in a 16:9 frame, remote control, terminal, power state, firmware updater for the RT4K, Cruller OTA upload. Live data over a WebSocket (`/ws`); the page never polls. Page code kept as real files in `src/web` is embedded at build time (`src/web/embed.cmake`).
- **`POST /api/command`**: console commands in (`{"command"}`, `{"commands": []}`, `{"button"}` with hass-RT4K's names, or plain text lines), their own replies out once each window closes.
- **RFC 2217** on TCP port 2217 (pyserial's `rfc2217://`, e.g. Home Assistant's hass-RT4K): each client sees only its own replies; TCP keepalive drops clients that vanished.
- **Client budget:** web pages and RFC 2217 clients share 8 slots, in any mix (`src/core/clients.h`). When full, a newcomer replaces one of its own kind (the quietest page, the oldest RFC 2217 client), or is turned away (a page gets 503) rather than taking a live client of the other kind. lwIP is sized for that plus HTTP: 20 sockets, 24 TCP connections, 32 KB heap. `GET /debug/memory` shows the use and peaks.
- **`POST /rt4k/put`, `POST /rt4k/ask`**: file uploads to the RT4K's SD card and single queries, used by the firmware updater.
- **RT4K firmware updates**: the page reads RetroTINK's firmware index on GitHub, downloads the zip, checks it against the SHA-256 in the index, unzips it in the browser, writes the files through Cruller and runs `fwup check` / `fwup go`.
- **Status**: `GET /status` (JSON, also pushed over the WebSocket).
- **Debug routes** (not for automations): `/debug/tasks` (`?stacks`), `/debug/memory`, `/debug/console`, `/debug/usbtrace`, `/debug/freeze`, `/debug/lastfail`, `POST /debug/raw`, `/debug/flow`, `/debug/baud`, `/debug/gap`.

## Flash layout

DonutShop Pico layout (arduino-pico, `flash=4194304_2097152`):

| Flash offset | Content |
| --- | --- |
| `0x000000`–`0x003000` | arduino-pico OTA stage-3 (12 KB) |
| `0x003000`–`0x1FE000` | DonutShop application |
| `0x1FF000`–`0x3FF000` | LittleFS: `wifi.json`, `consoles.json`, `gameDB.json`, `settings.json` |
| `0x3FF000`–`0x400000` | EEPROM emulation |

Cruller layout (`src/platform/rp2/pt.json`). The RP2350 boot ROM reads the partition table and does A/B selection:

| Flash offset | Size | Content |
| --- | --- | --- |
| `0x000000` | 8 KB | partition table |
| `0x002000` | 1856 KB | slot A |
| `0x1D2000` | 1856 KB | slot B |
| after B | rest | data (littlefs: configuration) |

## Migration from DonutShop (first install)

DonutShop's `/update` stores the uploaded file as `firmware.bin` in its LittleFS. On reboot, its OTA stage-3 copies itself to RAM and writes the file to flash from `0x000000`. It doesn't check what the image contains; DonutShop only rejects ESP32 images (first byte `0xE9`).

So the migration image is the Cruller flash image from offset 0: the partition table followed by slot A. It must stay below the old LittleFS while being copied out of it. It can be installed through the DonutShop web UI (manual firmware upload), or through DonutShop's GitHub updater (a release with the asset `DonutShop_v<version>_pico2w_update.bin` and a version above 0.6.3).

On its first boot, Cruller finds no data partition and runs the migration: it mounts the old LittleFS read-only, reads the configuration, formats the data partition and writes the converted configuration (credentials first).

Risk windows: while the DonutShop stage-3 overwrites its own first 12 KB (milliseconds, once; losing power there needs BOOTSEL recovery), and between reading the old configuration and writing the new one (losing power loses the configuration, not the board: Cruller falls back to the provisioning portal).

## OTA (Cruller to Cruller)

- **Upload** from the web page or `curl --data-binary @build/rp2/cruller.uf2 http://<board>/update`; images over 1 MiB make curl send `Expect: 100-continue`, which Cruller answers.
- **Write:** straight into the inactive slot, a sector at a time, with the RT4K's USB host quiet meanwhile (`flash_quiet_begin`).
- **Switch:** the boot ROM's "try before you buy" flow. After a reboot into the new slot, the image is confirmed once healthy; otherwise the boot ROM's watchdog returns to the previous slot. Cruller's own watchdog stays off during the trial and takes over after the confirmation.
- **Reboots:** power the CYW43 down (`WL_REG_ON` low) and stop feeding the watchdog before every software reboot (feeding it postpones a scheduled reboot).
- **Versions** (`CRULLER_VERSION`) must grow with every image: the boot ROM chooses between A and B by version.

## Networking

- **Provisioning:** an access point with DHCP and DNS captive portal, listing nearby networks.
- **Station:** join with a timeout, reconnect in the background, and fall back to the portal after repeated failures. Never block forever.
- **mDNS:** `cruller.local`.
- **Health:** gateway pings every 2 s; no reply for 10 s stops feeding the watchdog (the network can die while everything else runs). To be made configurable.

## Configuration

JSON files in littlefs, with a schema version. The importer reads the DonutShop formats (`consoles.json`, `gameDB.json`, `settings.json`). Wi-Fi credentials are stored separately and are never included in configuration exports.

## Milestones

1. **M0, foundation (done 2026-09-25):** CMake project, flash layout with partition table, migration from DonutShop by OTA with the Wi-Fi credentials, station mode with the portal fallback, mDNS, Cruller-to-Cruller OTA with rollback.
2. **M1, RT4K link (done):** USB host FTDI at 2 Mbaud, two-way, hot-plug, web terminal, RTL1 transfers. HD-15 still to do.
3. **M2, gameID:** console polling (HTTP and HTTPS), gameDB, profile switching with DonutShop's rules (SRS/S0), and the configuration UI. Not started.
4. **M3, control (mostly done):** remote-control page with the screen mirror, power state, `/api/command`, RFC 2217. LED patterns still to do.
5. **M4, extras (partly done):** RT4K SD file transfers and firmware updates from RetroTINK's repository. Still to do: Extron/TESmart/MT-VIKI serial, IR, profiles.

### M0 results (2026-09-25)

- **Migration:** DonutShop accepted `cruller_migration.bin` through `/update`. Cruller booted from partition A, imported `wifi.json` and kept its IP. The first upload attempt hung inside DonutShop and needed a power cycle; the retry worked.
- **OTA:** 0.0.1 to 0.0.2 went to partition B (6 s upload, back in 16 s). 0.0.2 to 0.0.3 went back to A. After a power cycle the board booted 0.0.3 from A with a normal boot, so the explicit buy persisted.

## Testing

- **On the host** (`tests/run.sh`, gcc and node): the RTL1 engine, WebSocket framing, power state, RFC 2217 parsing, console reply windows, the page's JavaScript (syntax), and the RT4K firmware updater's logic (SHA-256, RetroTINK's index format, zip reading; optionally a real firmware zip).
- **On the bench:** `cruller_bench` (not built by default) has the USB port as a serial console and no RT4K link.
- **On the board behind the RT4K:** OTA only. `/debug/tasks` has the link counters (FT232R overruns, key -> screen times), `/debug/console` the last commands' reply times, and `/debug/freeze` where both cores were before a watchdog reset (kept across it).

## Open issues

- An OTA upload now and then hangs right at its start (the board stops answering; the watchdog resets it into the old image and a retry works). Cause unknown.
- A few unexplained watchdog resets on 0.0.82/0.0.83; the freeze record showed both cores idle and sampling up to the reset. Not seen since.

## Lessons

From the DonutShop port:

- The arduino-pico FreeRTOS layer forwarded single lwIP calls to its lwIP task but didn't protect socket state: races on the receive chain, on pcb lifetime and on accept queues. Cruller uses lwIP core locking instead.
- The arduino-pico CYW43 IRQ poll took the driver mutex with a 1-tick timeout and used the bus anyway when that failed. Cruller relies on the SDK's `cyw43_arch` locking.
- A software reset can leave the CYW43 in a state from which it doesn't come up properly. Power it off before rebooting.
- Never read network data a byte at a time across a thread boundary (171 s for an OTA upload, 12 s with 256-byte reads).
- A USB cable without data lines looks exactly like "no device".

From Cruller:

- The FT232R buffers only ~1.3 ms of data at 2 Mbaud and hands over short packets, so every packet costs a USB transfer round trip. Code running from flash went cold in the XIP cache and missed that deadline; the receive path runs from RAM now. The FTDI status bytes (overrun flag) are the way to tell.
- `uxTaskGetSystemState()` suspends the scheduler while it scans every stack: polling it stalled the USB task for milliseconds.
- The RT4K ignores a transfer request sent before it has answered the last console command; right after the answer it's fine.
- Waiting a fixed time for replies wastes most of it: the RT4K answers in 1–17 ms, and nearly every reply has a known last line.
