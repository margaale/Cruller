# Cruller

Firmware for the Raspberry Pi Pico 2 W that controls a RetroTINK 4K over Wi-Fi. It is built on the Pico SDK, FreeRTOS, lwIP and TinyUSB, and updates over the air.

It takes the idea of [DonutShop](https://github.com/svirant/DonutShop), and can be installed over the air on a board that runs the DonutShop Pico 2 W firmware.

What it does today, with the Pico plugged into the RT4K's USB-C port:

- **Web page:** a live mirror of the RT4K's on-screen menu, a remote control, a terminal, and the RT4K's power state.
- **RT4K firmware updates** straight from RetroTINK's repository: pick a release or experimental version, and Cruller downloads, checks and installs it. No SD card swapping.
- **Automation:** `POST /api/command` for scripts and Home Assistant, and an RFC 2217 serial port on TCP 2217 (for example hass-RT4K with `rfc2217://cruller.local:2217`). Several clients can share the RT4K; each gets only the replies to its own commands.
- **Over-the-air updates** of Cruller itself, with rollback if a new image doesn't come up healthy.

Planned: automatic profile switching from the gameID your consoles report, and the HD-15 serial link.

See [docs/DESIGN.md](docs/DESIGN.md) for how it works and [docs/RTL1.md](docs/RTL1.md) for the RT4K's serial protocol as Cruller uses it.

## Building

`scripts/build.sh [rp2] [Debug|Release]` or `scripts/build.sh esp32` (in an ESP-IDF 6.1 shell) builds a target (`src/platform/<target>`) into `build/<target>`; paths default to the author's machine, override them through the environment. Host tests: `tests/run.sh`.

## Releases

Work goes to `develop` (the default branch) through pull requests; `master` takes what is released. Versions come from GitVersion (`GitVersion.yml`), and CI builds every board with them:

- **`master`:** every push is a release, tagged `vX.Y.Z`, with every board's images. The patch grows with each one; a line `+semver: minor` in a commit message bumps the minor.
- **`develop`:** every push is a pre-release, `vX.Y.Z-alpha.N`, to try on a board. N is CI's run number, which grows with every build on every branch: a newer build is always a newer version, and a release sorts after its pre-releases. The Cruller tab lists alphas but suggests them only to a board already running one.
- **Pull requests** (into `develop`): built as `X.Y.Z-pr.N`, not published; the images are on the run, one file each.

The ESP32-S3 images include `cruller-factory.bin`, to flash a new board over USB at 0x0.

## License

GPL-3.0. See [LICENSE](LICENSE).
