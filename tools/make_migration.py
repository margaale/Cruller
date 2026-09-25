#!/usr/bin/env python3
"""Builds the image that migrates a DonutShop Pico 2 W board to Cruller.

DonutShop (arduino-pico) stages an uploaded .bin in its LittleFS and its OTA stage-3 copies it to
flash starting at offset 0. This image is therefore the flash contents from offset 0: the
partition table, padding, and the Cruller application at the start of partition A.

usage: make_migration.py <pt.bin> <app.bin> <out.bin> <partition A offset> <max size>
"""
import sys


def main() -> int:
    pt_path, app_path, out_path = sys.argv[1:4]
    part_a = int(sys.argv[4], 0)
    max_size = int(sys.argv[5], 0)

    pt = open(pt_path, "rb").read()
    app = open(app_path, "rb").read()
    if len(pt) > part_a:
        sys.exit(f"partition table ({len(pt)} bytes) overlaps partition A at {part_a:#x}")
    image = pt + b"\xff" * (part_a - len(pt)) + app
    if len(image) > max_size:
        # The stage-3 copies from DonutShop's LittleFS, which starts at max_size: it must not be overwritten.
        sys.exit(f"migration image is {len(image):#x} bytes, limit {max_size:#x}")
    if image[0] == 0xE9:
        sys.exit("migration image starts with 0xE9: DonutShop would reject it as an ESP32 image")
    open(out_path, "wb").write(image)
    print(f"{out_path}: {len(image)} bytes (partition table + app at {part_a:#x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
