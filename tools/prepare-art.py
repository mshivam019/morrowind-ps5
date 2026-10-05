#!/usr/bin/env python3
"""Build matching PS5 selection and launch artwork.

Run with Pillow and ispc_texcomp installed. Outputs single-image 4K BC7 DDS.
"""
from pathlib import Path
import struct
import shutil

from PIL import Image
import ispc_texcomp

ROOT = Path(__file__).resolve().parents[1]
SIZE = (3840, 2160)


def write_dds(image, target):
    width, height = image.size
    surface = ispc_texcomp.RGBASurface(image.tobytes(), width, height, width * 4)
    blocks = ispc_texcomp.compress_blocks_bc7(
        surface, ispc_texcomp.BC7EncSettings.from_profile("slow"))
    assert len(blocks) == width * height
    header = bytearray(148)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x81007, height, width, len(blocks), 0, 1)
    struct.pack_into("<2I4s", header, 76, 32, 4, b"DX10")
    struct.pack_into("<I", header, 108, 0x1000)
    struct.pack_into("<5I", header, 128, 98, 3, 0, 1, 3)
    target.write_bytes(header + blocks)
    print(target)


def main():
    with Image.open(ROOT / "art/background-source.png") as source:
        background = source.convert("RGBA").resize(SIZE, Image.Resampling.LANCZOS)
    write_dds(background, ROOT / "art/pic0.dds")
    shutil.copyfile(ROOT / "art/pic0.dds", ROOT / "art/pic1.dds")


if __name__ == "__main__":
    main()
