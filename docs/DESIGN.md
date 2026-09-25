# Cruller design

Cruller is firmware for the Raspberry Pi Pico 2 W that controls a RetroTINK 4K (RT4K) over Wi-Fi. It switches RT4K profiles from the gameID that consoles report, and provides a web UI to control the RT4K. It takes the idea of [DonutShop](https://github.com/svirant/DonutShop) and is written from scratch on the Pico SDK, without Arduino.

Status: M0 done (2026-09-25). The first image was installed over the air on a DonutShop board behind an RT4K, then updated A to B to A and survived a power cycle.

## Goals

- **Over-the-air updates from the first image.** The board sits behind the RT4K and should never need a USB cable after the first install.
- **Installable by OTA from the DonutShop Pico 2 W firmware** (the arduino-pico port in margaale/DonutShop), keeping the user's Wi-Fi credentials and gameID configuration.
- **Robust networking.** Use the Pico SDK's own FreeRTOS + lwIP + CYW43 integration, with lwIP core locking. The arduino-pico FreeRTOS layers showed races, a skipped CYW43 mutex and 8–15 s CYW43 bring-ups (see "Lessons from the DonutShop port").
- **A two-way RT4K link over USB** (the RT4K is an FTDI FT232R, 0403:6001, at 2 Mbaud) and over the HD-15 serial port.

Non-goals for now: other scalers, other boards, and reusing the DonutShop v0.7 web UI (its source isn't published).

## Hardware

Raspberry Pi Pico 2 W (RP2350, 520 KB RAM, 4 MB flash, CYW43439). The pinout is the same as the DonutShop port, so existing boards keep working:

| Function | Pins | Notes |
| --- | --- | --- |
| RT4K USB | native micro-USB, USB host | micro-USB OTG adapter that also powers the bus |
| RT4K HD-15 serial | UART1 TX GP8, RX GP9 | pull-ups to 3.3 V |
| Extron sw1 | UART0 TX GP0, RX GP1 | |
| Extron sw2 | PIO UART TX GP4, RX GP5 | |
| IR receiver / emitter | GP2 / GP3 | optional |
| RGB status LED | GP16 / GP17 / GP18 | optional, active low |
| Status LED | CYW43 GPIO 0 | on-board |

## Software stack

- Pico SDK 2.x, built with CMake. CI uses the official toolchain.
- FreeRTOS-Kernel SMP (the SDK's RP2350 port).
- lwIP with `pico_cyw43_arch_lwip_sys_freertos`: lwIP runs in its own tcpip thread, and application code takes the lwIP core lock (`LWIP_TCPIP_CORE_LOCKING`). That lock is the only way into lwIP.
- The CYW43 driver through `pico_cyw43_arch`: its thread lock is the SDK's, not a custom one.
- TinyUSB in host mode on the native port, with CDC-ACM and the FTDI/CP210x/CH34x class drivers.
- littlefs for configuration, mbedTLS through `altcp_tls` for outgoing HTTPS (MemCardPro 2 firmware), and lwIP's mDNS responder.

## Tasks

| Task | Core | Role |
| --- | --- | --- |
| tcpip (lwIP) | any | lwIP thread, owned by `pico_cyw43_arch` |
| net | any | Wi-Fi state machine: join, watchdog, fallback to the provisioning portal, mDNS |
| http | any | HTTP server and WebSocket (UI, API, terminal) |
| rt4k | 1 | TinyUSB host task, RT4K TX/RX queues, HD-15 UART |
| gameid | any | polls consoles over HTTP(S) and decides profile changes |
| ota | any | downloads and writes images to the inactive slot |
| led | any | status LED patterns |

Queues and stream buffers connect the tasks. Only the rt4k task touches TinyUSB, and only lwIP-locked code touches lwIP.

## RT4K link

- **USB:** the RT4K enumerates as an FTDI FT232R at 2 Mbaud. The link is two-way from the start, because a terminal and later WebCtl-style features need replies. Hot-plug has to work: the link is up when the device mounts and down when it unmounts.
- **HD-15:** 9600 baud by default, 2 Mbaud with the RT4K "baud mod".
- **Commands:** the public remote-control command set (`remote ...`, `SVS NEW INPUT=n`, `pwr on`, ...). The firmware 1.75+ console commands (`ls`, `struct`, `osd2 state`, ...) come later, once they are documented.

## Flash layout

DonutShop Pico layout (arduino-pico, `flash=4194304_2097152`):

| Flash offset | Content |
| --- | --- |
| `0x000000`–`0x003000` | arduino-pico OTA stage-3 (12 KB) |
| `0x003000`–`0x1FE000` | DonutShop application |
| `0x1FF000`–`0x3FF000` | LittleFS: `wifi.json`, `consoles.json`, `gameDB.json`, `settings.json` |
| `0x3FF000`–`0x400000` | EEPROM emulation |

Cruller layout. The RP2350 boot ROM reads a partition table and does A/B selection:

| Flash offset | Size | Content |
| --- | --- | --- |
| `0x000000` | 4 KB | partition table |
| `0x001000` | ~1.9 MB | slot A |
| `0x1E0000` | ~1.9 MB | slot B |
| `0x3C0000` | 256 KB | data (littlefs: configuration) |

The exact numbers get settled in M0, with `picotool partition` and a real image size.

## Migration from DonutShop (first install)

DonutShop's `/update` stores the uploaded file as `firmware.bin` in its LittleFS. On reboot, its OTA stage-3 copies itself to RAM and writes the file to flash from `0x000000`. It doesn't check what the image contains; DonutShop only rejects ESP32 images (first byte `0xE9`).

So the migration image is the Cruller flash image from offset 0: the partition table followed by slot A. It must be smaller than ~1.9 MB, so that it stays below the old LittleFS while being copied out of it. It can be installed:

- through the DonutShop web UI (manual firmware upload), or
- through DonutShop's GitHub updater, by publishing a release on the repository the firmware checks, with the asset `DonutShop_v<version>_pico2w_update.bin` and a version above 0.6.3.

On its first boot, Cruller finds no data partition and runs the migration:

1. Mount the old LittleFS (`0x1FF000`, 2 MB, 4 KB blocks) read-only, and read the four JSON files into RAM.
2. Format the data partition and write the converted configuration (credentials first).
3. Start normally. Slot B and the rest of the old filesystem get overwritten by the first Cruller OTA.

Risk windows:

- **While the DonutShop stage-3 overwrites its own first 12 KB** (milliseconds, once). Losing power there needs BOOTSEL recovery. Nothing in Cruller can remove this window.
- **Between reading the old configuration and writing the new one.** Losing power there loses the configuration, not the board: Cruller falls back to the provisioning portal.

Rehearse the migration on a bench board with a USB console, including deliberate power cuts, before touching a board installed behind an RT4K.

## OTA (Cruller to Cruller)

- **Sources:** upload from the web UI, and GitHub releases (checked from the UI, optionally periodically).
- **Download:** stream the image straight into the inactive slot, a sector at a time, with no RAM or filesystem staging. Hash it with SHA-256 while it arrives and compare against the release digest. Image signing is an option for later.
- **Switch:** use the boot ROM's "try before you buy" flow. After a reboot into the new slot, the firmware confirms the image only once it's healthy (Wi-Fi joined, HTTP server up, RT4K task running). Otherwise the next reboot, or the watchdog, returns to the previous slot.
- **Reboots:** power the CYW43 down (`WL_REG_ON` low for more than 100 ms) before every software reboot.

## Networking

- **Provisioning:** an access point with DHCP and DNS captive portal (the SDK examples' `dhcpserver`/`dnsserver`), listing nearby networks.
- **Station:** join with a timeout, reconnect in the background, and fall back to the portal after repeated failures. Never block forever.
- **mDNS:** `cruller.local`, configurable.
- **HTTP server:** static UI assets stored gzipped in flash, a small JSON API, and a WebSocket for the RT4K terminal and live status.

## Configuration

JSON files in littlefs, with a schema version. The importer reads the DonutShop formats (`consoles.json`, `gameDB.json`, `settings.json`). Wi-Fi credentials are stored separately and are never included in configuration exports.

## Milestones

1. **M0, foundation (done):** CMake project, CI, and the flash layout with partition table. Migration from DonutShop by OTA, including the import of Wi-Fi credentials. Station mode with the portal fallback, mDNS, a minimal web page, and Cruller-to-Cruller OTA with rollback. Done when a board goes from DonutShop to Cruller with no cable and then updates itself twice.
2. **M1, RT4K link:** USB host FTDI at 2 Mbaud, two-way, hot-plug, plus the HD-15 UART and a web terminal.
3. **M2, gameID:** console polling (HTTP and HTTPS), gameDB, profile switching with DonutShop's rules (SRS/S0), and the configuration UI.
4. **M3, control:** remote-control page, status LED patterns, and the RGB LED.
5. **M4, extras:** Extron/TESmart/MT-VIKI serial, IR, and SD file management once the RT4K protocol is documented.

### M0 results (2026-09-25)

- **Migration:** DonutShop accepted `cruller_migration.bin` through `/update`. Cruller booted from partition A, imported `wifi.json` and kept its IP. The first upload attempt hung inside DonutShop and needed a power cycle; the retry worked. Retrying is the documented recovery.
- **OTA:** 0.0.1 to 0.0.2 went to partition B (6 s upload, back in 16 s). 0.0.2 to 0.0.3 went back to A. After a power cycle the board booted 0.0.3 from A with a normal boot, so the explicit buy persisted.
- **Image versions** (`CRULLER_VERSION`) must grow with every image, because the boot ROM chooses between A and B by version.

## Testing

- **On the host:** gameID matching, profile rules, the configuration importer, and parsers, built with the host toolchain in CI.
- **On the bench:** a board on USB with a debug console, and scripts that loop reboots, joins, OTA updates and portal entries, then report failures and timings.
- **On the board behind the RT4K:** OTA only.

## Lessons from the DonutShop port

- The arduino-pico FreeRTOS layer forwarded single lwIP calls to its lwIP task but didn't protect socket state. The result was races on the receive chain, on pcb lifetime and on accept queues, plus a missing `netif_set_default` case. Cruller uses lwIP core locking instead.
- The arduino-pico CYW43 IRQ poll took the driver mutex with a 1-tick timeout and used the bus anyway when that failed. This made joins and the soft AP flaky, and the bring-up slow. Cruller relies on the SDK's `cyw43_arch` locking.
- A software reset can leave the CYW43 in a state from which it doesn't come up properly. Power it off before rebooting.
- Wi-Fi joins need an external watchdog.
- Never read network data a byte at a time across a thread boundary. The DonutShop OTA upload took 171 s because of that, and 12 s with 256-byte reads.
- A USB cable without data lines looks exactly like "no device": `line_state` stays SE0.
