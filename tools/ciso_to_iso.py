"""Converts a compressed GameCube disc (.ciso) into the plain .iso this port reads.

CISO keeps a 0x8000-byte header: "CISO", the block size (little endian), then one byte per block
saying whether the block is stored. Stored blocks follow in order; missing ones are all zeros.
The output is padded to the full GameCube disc size, which is what an ordinary Melee ISO is.

    python tools/ciso_to_iso.py <game.ciso> <out.iso>

Does nothing if <out.iso> already holds Melee NTSC 1.02, so build.bat and play.bat can call it
every time. Needs about 1.4 GB free next to the output.
"""
import os
import struct
import sys

GC_DISC_SIZE = 1_459_978_240
HEADER_SIZE = 0x8000


def is_melee_iso(path):
    try:
        with open(path, "rb") as f:
            head = f.read(8)
        return head[:6] == b"GALE01" and head[7] == 2 and os.path.getsize(path) >= 0x440
    except OSError:
        return False


def convert(src, dst):
    if is_melee_iso(dst):
        print(f"{dst}: already a Melee NTSC 1.02 ISO, using it")
        return
    with open(src, "rb") as f:
        header = f.read(HEADER_SIZE)
        if header[:4] != b"CISO":
            raise SystemExit(f"{src}: not a CISO file")
        block_size = struct.unpack("<I", header[4:8])[0]
        if block_size == 0 or block_size > 0x10000000:
            raise SystemExit(f"{src}: bad CISO block size {block_size}")
        present = header[8:HEADER_SIZE]
        last = max((i for i, b in enumerate(present) if b), default=-1)
        tmp = dst + ".tmp"
        zeros = bytes(block_size)
        with open(tmp, "wb") as out:
            for i in range(last + 1):
                if present[i]:
                    block = f.read(block_size)
                    if len(block) < block_size:
                        block += bytes(block_size - len(block))   # a short final block
                    out.write(block)
                else:
                    out.write(zeros)
                if i % 256 == 0:
                    print(f"\rconverting: {100 * (i + 1) // (last + 1)}%", end="", flush=True)
            if out.tell() < GC_DISC_SIZE:
                out.truncate(GC_DISC_SIZE)
        print("\rconverting: 100%")
    if not is_melee_iso(tmp):
        os.remove(tmp)
        raise SystemExit(f"{src}: converted, but it is not Melee NTSC 1.02 (GALE01 revision 2)")
    os.replace(tmp, dst)
    print(f"{src} -> {dst}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    convert(sys.argv[1], sys.argv[2])
