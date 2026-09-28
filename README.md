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

Every push to `main` is a release: CI takes the version from GitVersion (`GitVersion.yml`; the patch grows with each release, a line `+semver: minor` in a commit message bumps the minor), builds every board with it, tags `vX.Y.Z` and publishes the images on the GitHub release. Pull requests build alphas (`X.Y.Z-alpha.N`, the next release and the commits since the last one) to try on a board: each sorts after the one before and before its release. Every push to a pull request publishes one as a GitHub pre-release, and the images are also on the run as plain files (not zipped). The Cruller tab lists alphas but suggests them only to a board already running one. The ESP32-S3 images include `cruller-factory.bin`, to flash a new board over USB at 0x0.

## License

GPL-3.0. See [LICENSE](LICENSE).
