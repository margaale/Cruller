#!/usr/bin/env python3
"""Builds the factory image: a new board's flash contents from offset 0, the partition table, padding,
and Cruller at the start of slot A. CMake turns it into cruller-factory.uf2.

usage: make_factory.py <pt.bin> <app.bin> <out.bin> <slot A offset> <slot B offset>
"""
import sys


def main() -> int:
    pt_path, app_path, out_path = sys.argv[1:4]
    part_a = int(sys.argv[4], 0)
    part_b = int(sys.argv[5], 0)

    pt = open(pt_path, "rb").read()
    app = open(app_path, "rb").read()
    if len(pt) > part_a:
        sys.exit(f"partition table ({len(pt)} bytes) overlaps slot A at {part_a:#x}")
    image = pt + b"\xff" * (part_a - len(pt)) + app
    if len(image) > part_b:
        sys.exit(f"the app ({len(app):#x} bytes) doesn't fit in slot A ({part_b - part_a:#x})")
    open(out_path, "wb").write(image)
    print(f"{out_path}: {len(image)} bytes (partition table + app at {part_a:#x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
