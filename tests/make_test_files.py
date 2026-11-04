#!/usr/bin/env python3
"""Generate test files for Nintendall.

- test.nes: a tiny original NES program (stripes, a 440 Hz tone, holding A
  turns the background red). Used to check video, audio and input end to end.
- Fake header-only files for every other system, to test auto-detection,
  including misnamed files (the header must win over the extension).

usage: python3 tests/make_test_files.py OUTPUT_DIR
"""
import os
import struct
import sys
import zipfile


# ----------------------------------------------------------------------------
# Minimal 6502 assembler (only the opcodes the test program uses)

OPS = {
    ("sei", "impl"): 0x78, ("cld", "impl"): 0xD8, ("txs", "impl"): 0x9A, ("inx", "impl"): 0xE8,
    ("iny", "impl"): 0xC8, ("txa", "impl"): 0x8A, ("pha", "impl"): 0x48, ("pla", "impl"): 0x68,
    ("rti", "impl"): 0x40, ("lsr", "impl"): 0x4A,
    ("lda", "imm"): 0xA9, ("ldx", "imm"): 0xA2, ("ldy", "imm"): 0xA0, ("and", "imm"): 0x29,
    ("cpx", "imm"): 0xE0, ("cpy", "imm"): 0xC0,
    ("lda", "abs"): 0xAD, ("sta", "abs"): 0x8D, ("stx", "abs"): 0x8E, ("bit", "abs"): 0x2C,
    ("jmp", "abs"): 0x4C, ("lda", "absx"): 0xBD,
    ("bpl", "rel"): 0x10, ("bne", "rel"): 0xD0, ("beq", "rel"): 0xF0,
}
SIZE = {"impl": 1, "imm": 2, "abs": 3, "absx": 3, "rel": 2}


def assemble(program, origin):
    labels, pc = {}, origin
    for item in program:
        if isinstance(item, str):
            labels[item] = pc
        elif item[0] == ".bytes":
            pc += len(item[1])
        else:
            pc += SIZE[item[1]]
    out, pc = bytearray(), origin
    for item in program:
        if isinstance(item, str):
            continue
        if item[0] == ".bytes":
            out += bytes(item[1])
            pc += len(item[1])
            continue
        op, mode = item[0], item[1]
        arg = item[2] if len(item) > 2 else None
        if isinstance(arg, str):
            arg = labels[arg]
        out.append(OPS[(op, mode)])
        if mode == "imm":
            out.append(arg & 0xFF)
        elif mode in ("abs", "absx"):
            out += struct.pack("<H", arg)
        elif mode == "rel":
            off = arg - (pc + 2)
            assert -128 <= off <= 127, "branch out of range"
            out.append(off & 0xFF)
        pc += SIZE[mode]
    return bytes(out), labels


def make_nes():
    def sta(addr):
        return ("sta", "abs", addr)

    def set_bg(color):
        return [("lda", "imm", 0x3F), sta(0x2006), ("lda", "imm", 0x00), sta(0x2006),
                ("lda", "imm", color), sta(0x2007)]

    palette = [0x0F, 0x30, 0x21, 0x11, 0x0F, 0x27, 0x17, 0x07,
               0x0F, 0x2A, 0x1A, 0x0A, 0x0F, 0x24, 0x14, 0x04] * 2
    prog = [
        "reset",
        ("sei", "impl"), ("cld", "impl"),
        ("ldx", "imm", 0x40), ("stx", "abs", 0x4017),
        ("ldx", "imm", 0xFF), ("txs", "impl"), ("inx", "impl"),
        ("stx", "abs", 0x2000), ("stx", "abs", 0x2001), ("stx", "abs", 0x4010),
        "vwait1", ("bit", "abs", 0x2002), ("bpl", "rel", "vwait1"),
        "vwait2", ("bit", "abs", 0x2002), ("bpl", "rel", "vwait2"),
        # palette
        ("lda", "imm", 0x3F), sta(0x2006), ("lda", "imm", 0x00), sta(0x2006),
        ("ldx", "imm", 0),
        "pal", ("lda", "absx", "palette"), sta(0x2007), ("inx", "impl"), ("cpx", "imm", 0x20),
        ("bne", "rel", "pal"),
        # nametable + attributes: 4 pages of (x>>2)&3
        ("lda", "imm", 0x20), sta(0x2006), ("lda", "imm", 0x00), sta(0x2006),
        ("ldy", "imm", 0), ("ldx", "imm", 0),
        "fill", ("txa", "impl"), ("lsr", "impl"), ("lsr", "impl"), ("and", "imm", 3), sta(0x2007),
        ("inx", "impl"), ("bne", "rel", "fill"), ("iny", "impl"), ("cpy", "imm", 4), ("bne", "rel", "fill"),
        # 440 Hz square wave on pulse 1
        ("lda", "imm", 0x01), sta(0x4015),
        ("lda", "imm", 0xBF), sta(0x4000),
        ("lda", "imm", 0x08), sta(0x4001),
        ("lda", "imm", 0xFD), sta(0x4002),
        ("lda", "imm", 0x00), sta(0x4003),
        # scroll 0, NMI on, background on
        ("lda", "imm", 0), sta(0x2005), sta(0x2005),
        ("lda", "imm", 0x80), sta(0x2000),
        ("lda", "imm", 0x0A), sta(0x2001),
        "forever", ("jmp", "abs", "forever"),

        "nmi", ("pha", "impl"),
        ("lda", "imm", 1), sta(0x4016), ("lda", "imm", 0), sta(0x4016),
        ("lda", "abs", 0x4016), ("and", "imm", 1), ("beq", "rel", "not_pressed"),
        *set_bg(0x16), ("jmp", "abs", "done"),
        "not_pressed", *set_bg(0x0F),
        "done", ("lda", "imm", 0), sta(0x2005), sta(0x2005), ("lda", "imm", 0x80), sta(0x2000),
        ("pla", "impl"), ("rti", "impl"),
        "irq", ("rti", "impl"),
        "palette", (".bytes", palette),
    ]
    code, labels = assemble(prog, 0xC000)
    prg = bytearray(code.ljust(0x4000 - 6, b"\xEA"))
    prg += struct.pack("<HHH", labels["nmi"], labels["reset"], labels["irq"])

    chr_rom = bytearray(0x2000)
    for tile, (p0, p1) in enumerate([(0x00, 0x00), (0xFF, 0x00), (0x00, 0xFF), (0xFF, 0xFF)]):
        chr_rom[tile * 16:tile * 16 + 8] = bytes([p0]) * 8
        chr_rom[tile * 16 + 8:tile * 16 + 16] = bytes([p1]) * 8

    header = b"NES\x1A" + bytes([1, 1, 0, 0]) + bytes(8)  # 16K PRG, 8K CHR, mapper 0
    return header + bytes(prg) + bytes(chr_rom)


# ----------------------------------------------------------------------------
# Fake headers for detection tests

GB_LOGO = bytes.fromhex(
    "CEED6666CC0D000B03730083000C000D0008111F8889000EDCCC6EE6DDDDD999BBBB67636E0EECCCDDDC999FBBB9333E")
GBA_LOGO_START = bytes.fromhex("24FFAE51699AA2213D84820A")


def gb(cgb_flag):
    rom = bytearray(0x8000)
    rom[0x104:0x134] = GB_LOGO
    rom[0x134:0x143] = b"NINTENDALLTEST\0"
    rom[0x143] = cgb_flag
    x = 0
    for i in range(0x134, 0x14D):
        x = (x - rom[i] - 1) & 0xFF
    rom[0x14D] = x
    return bytes(rom)


def gba():
    rom = bytearray(0x1000)
    rom[0x04:0x04 + len(GBA_LOGO_START)] = GBA_LOGO_START
    rom[0xA0:0xAC] = b"NINTENDALLGB"
    rom[0xB2] = 0x96
    c = 0
    for i in range(0xA0, 0xBD):
        c = (c - rom[i]) & 0xFF
    rom[0xBD] = (c - 0x19) & 0xFF
    return bytes(rom)


def nds():
    rom = bytearray(0x4000)
    rom[0:12] = b"NINTENDALLDS"
    rom[0xC0:0xC0 + len(GBA_LOGO_START)] = GBA_LOGO_START
    rom[0x15C:0x15E] = struct.pack("<H", 0xCF56)
    return bytes(rom)


def snes(hirom, copier):
    size = 0x80000
    rom = bytearray(size)
    base = 0xFFC0 if hirom else 0x7FC0
    rom[base:base + 21] = b"NINTENDALL TEST SNES "
    rom[base + 0x15] = 0x21 if hirom else 0x20
    rom[base + 0x16] = 0x02
    rom[base + 0x17] = 0x09
    rom[base + 0x1C:base + 0x20] = struct.pack("<HH", 0xEDCB, 0x1234)
    rom[base + 0x3C:base + 0x3E] = struct.pack("<H", 0x8000)
    return (bytes(512) if copier else b"") + bytes(rom)


def disc(offset, magic, size=0x10000):
    d = bytearray(size)
    d[0:6] = b"GNTE01"
    d[offset:offset + 4] = struct.pack(">I", magic)
    return bytes(d)


def blob(prefix, size=0x1000, at=0):
    d = bytearray(size)
    d[at:at + len(prefix)] = prefix
    return bytes(d)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "testfiles"
    os.makedirs(out, exist_ok=True)

    def w(name, data):
        path = os.path.join(out, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    nes = make_nes()
    w("test.nes", nes)
    w("misnamed_really_nes.gba", nes)  # header must beat the extension
    with zipfile.ZipFile(os.path.join(out, "zipped_nes.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("readme.txt", "not a game")
        z.writestr("Test Game (USA).nes", nes)

    w("disk.fds", b"FDS\x1A" + bytes(12) + bytes([1]) + b"*NINTENDO-HVC*" + bytes(65500 - 15))
    w("snes_lorom.sfc", snes(False, False))
    w("snes_hirom_copier.smc", snes(True, True))
    w("mystery_snes.bin", snes(False, False))
    w("n64_big_endian.z64", blob(struct.pack(">I", 0x80371240)))
    w("n64_byteswapped.v64", blob(struct.pack(">I", 0x37804012)))
    w("n64_little_endian.n64", blob(struct.pack(">I", 0x40123780)))
    w("n64_wrong_ext.rom", blob(struct.pack(">I", 0x80371240)))
    w("pocket.gb", gb(0x00))
    w("color_only.gbc", gb(0xC0))
    w("color_compatible.gb", gb(0x80))
    w("advance.gba", gba())
    w("dual_screen.nds", nds())
    w("gamecube.iso", disc(0x1C, 0xC2339F3D))
    w("wii.iso", disc(0x18, 0x5D1C9EA3))
    w("wii_rvz.rvz", blob(b"RVZ\x01", 0x200) [:0x48] + struct.pack(">I", 2) + bytes(0x200))
    w("gamecube_rvz.rvz", blob(b"RVZ\x01", 0x200)[:0x48] + struct.pack(">I", 1) + bytes(0x200))
    w("wii_gcz.gcz", struct.pack("<II", 0xB10BC001, 1) + bytes(0x100))
    w("wii_backup.wbfs", blob(b"WBFS"))
    w("cart.3ds", blob(b"NCSD", 0x1000, 0x100))
    w("installable.cia", struct.pack("<I", 0x2020) + bytes(0x1000))
    w("homebrew.3dsx", blob(b"3DSX"))
    w("wiiu_disc.wux", blob(b"WUX0"))
    w("wiiu_disc.wud", blob(b"WUP-P-ABCD"))
    rpx = bytearray(0x100)
    rpx[0:4] = b"\x7FELF"
    rpx[4], rpx[5], rpx[7], rpx[8] = 1, 2, 0xCA, 0xFE
    w("game.rpx", bytes(rpx))
    w("Wii U Game Folder/code/game.rpx", bytes(rpx))
    w("Wii U Game Folder/meta/meta.xml", b"<menu/>")
    w("other_console.iso", bytes(0x10000))
    w("random_junk.bin", os.urandom(0x8000))
    w("notes.txt", b"hello")
    print("wrote test files to", out)


if __name__ == "__main__":
    main()
