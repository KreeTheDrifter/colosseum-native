#!/usr/bin/env python3
"""Pokemon Colosseum (USA) disc tools: CISO -> ISO, identity check, extraction.

  disc.py ciso2iso <in.ciso> <out.iso>
  disc.py verify   <game.iso>
  disc.py extract  <game.iso> <outdir>     -> outdir/sys/main.dol, outdir/files/...
  disc.py patch-dol <main.dol> <out.dol>   bake the code patches below into main.dol
  disc.py patches-h <out.h>                C++ header with the same patches (for the launcher)
"""
import hashlib, os, struct, sys

GC_DISC_SIZE = 1_459_978_240
GAME_ID = b"GC6E01"
MAIN_DOL_SHA1 = "870e8b9693ca780782d80f22a6a4572d8ba9458f"  # USA rev 0


# Code patches baked into main.dol before recompiling: (address, original word, new word).
# Recompiled code can't be patched in memory at run time, so fixes must go in here.
DOL_PATCHES = [
    # Dolphin GameSettings GC6E01 "Allow Memory Card saving with Savestates" (enabled by
    # default there and in ModernGekko's port tool). Without it, saving after loading a
    # save state can be refused as a different memory card.
    (0x801CFC2C, 0x418100AC, 0x9005002C),  # store the card's save counter instead of comparing
    (0x801CFC7C, 0x4082005C, 0x60000000),  # nop the second "same card" check

    # Settings-driven widescreen. C_MTXPerspective (0x800A3910) copies its aspect
    # argument with "fmr f29, f2" and divides by it for m[0][0]. That copy becomes a call
    # to a leaf in zeroed padding at 0x80005300 returning f29 = f2 * <aspect multiplier
    # from the settings block>, so every 3D camera renders at the chosen aspect ratio
    # (1.0 = original 4:3). Reworked from Dolphin's "16:9 Widescreen" code: a plain call
    # instead of a jump into the middle of a function, and a runtime setting.
    (0x800A3930, 0xFFA01090, 0x4BF619D1),  # fmr f29,f2  ->  bl 0x80005300
    (0x80005300, 0x00000000, 0x3D808026),  # lis   r12, 0x8026
    (0x80005304, 0x00000000, 0xC3AC6374),  # lfs   f29, 0x6374(r12)   (aspect multiplier)
    (0x80005308, 0x00000000, 0xEFA20772),  # fmuls f29, f2, f29
    (0x8000530C, 0x00000000, 0x4E800020),  # blr

    # Settings-driven frame rate. The end-of-frame routine passes the field's
    # frame-interval flag (1 = wait two retraces = 30 fps) to the frame limiter at
    # 0x800D361C. Route it through a leaf that ANDs it with the settings block's mask:
    # 0x00 -> always 0 (60 fps, as menus already run), 0xFF -> the game's own value.
    # From Dolphin's "60 FPS [Nerdzilla]" (which forces 0).
    (0x80005D98, 0x886D85B0, 0x4BFFF579),  # lbz r3,-0x7a50(r13) -> bl 0x80005310
    (0x80005310, 0x00000000, 0x886D85B0),  # lbz r3, -0x7a50(r13)   (game's flag)
    (0x80005314, 0x00000000, 0x3D808026),  # lis r12, 0x8026
    (0x80005318, 0x00000000, 0x880C6370),  # lbz r0, 0x6370(r12)    (frame-rate mask)
    (0x8000531C, 0x00000000, 0x7C630038),  # and r3, r3, r0
    (0x80005320, 0x00000000, 0x4E800020),  # blr
]

# Settings read by the patched code, stored in bytes of main.dol that nothing uses: the
# 0x20-byte data section at 0x80266360 is the static-constructor list, which
# __init_cpp (0x800A2BBC) walks until the first zero word, and its first word is zero.
# Data sections aren't part of ModernGekko's code verification, so the launcher can
# rewrite these bytes per launch without rebuilding. (0x80001800, tried first, is
# overwritten at run time.)
SETTINGS_ADDR = 0x80266370
SETTINGS_SIZE = 0x8
FPS_MASK_60 = 0x00        # +0x0 u8: 0x00 = 60 fps everywhere, 0xFF = original 30/60
FPS_MASK_ORIGINAL = 0xFF
ASPECTS = {"4:3": 1.0, "16:9": 4 / 3, "16:10": 1.2, "21:9": 1.75, "32:9": 8 / 3}  # +0x4 f32


def settings_block(fps60=True, aspect="16:9"):
    block = bytearray(SETTINGS_SIZE)
    block[0] = FPS_MASK_60 if fps60 else FPS_MASK_ORIGINAL
    block[4:8] = struct.pack(">f", ASPECTS[aspect])
    return bytes(block)


def write_settings(dol, block):
    """Write the settings bytes into main.dol (they're zero in the original)."""
    at = dol_offset(dol, SETTINGS_ADDR)
    if dol_offset(dol, SETTINGS_ADDR + SETTINGS_SIZE - 1) != at + SETTINGS_SIZE - 1:
        raise ValueError("settings range crosses a section boundary")
    dol[at:at + SETTINGS_SIZE] = block


def dol_offset(dol, address):
    offs = struct.unpack(">18I", dol[0:72])
    addrs = struct.unpack(">18I", dol[0x48:0x90])
    sizes = struct.unpack(">18I", dol[0x90:0xD8])
    for o, a, n in zip(offs, addrs, sizes):
        if n and a <= address < a + n:
            return o + address - a
    raise ValueError(f"{address:#x} is not in main.dol")


def patch_dol(src, dst):
    dol = bytearray(open(src, "rb").read())
    for address, old, new in DOL_PATCHES:
        at = dol_offset(dol, address)
        cur = struct.unpack(">I", dol[at:at + 4])[0]
        if cur not in (old, new):
            sys.exit(f"unexpected word {cur:#010x} at {address:#x}; is this the USA main.dol?")
        dol[at:at + 4] = struct.pack(">I", new)
    write_settings(dol, settings_block())
    open(dst, "wb").write(dol)
    print(f"patched {len(DOL_PATCHES)} words + settings block -> {dst}")


def patches_h(dst):
    """Write the patch list as a C++ header so the launcher patches main.dol identically."""
    lines = ["// Generated by tools/disc.py patches-h. Do not edit; change DOL_PATCHES instead.",
             "#pragma once", "#include <cstdint>", "",
             "struct DolPatch { uint32_t address, original, replacement; };",
             "inline constexpr DolPatch kDolPatches[] = {"]
    for a, old, new in DOL_PATCHES:
        lines.append(f"    {{0x{a:08X}u, 0x{old:08X}u, 0x{new:08X}u}},")
    lines.append("};")
    lines += ["", f"inline constexpr uint32_t kSettingsAddr = 0x{SETTINGS_ADDR:08X}u;",
              f"inline constexpr uint32_t kSettingsSize = 0x{SETTINGS_SIZE:X}u;",
              f"inline constexpr uint8_t kFpsMask60 = 0x{FPS_MASK_60:02X}u;",
              f"inline constexpr uint8_t kFpsMaskOriginal = 0x{FPS_MASK_ORIGINAL:02X}u;",
              "struct AspectChoice { const char* name; float multiplier; };",
              "inline constexpr AspectChoice kAspects[] = {"]
    lines += [f'    {{"{k}", {v!r}f}},' for k, v in ASPECTS.items()]
    lines.append("};")
    open(dst, "w").write("\n".join(lines) + "\n")
    print(f"wrote {len(DOL_PATCHES)} patches -> {dst}")


def ciso2iso(src, dst):
    with open(src, "rb") as f, open(dst, "wb") as out:
        hdr = f.read(0x8000)
        if hdr[:4] != b"CISO":
            sys.exit("not a CISO file")
        block = struct.unpack("<I", hdr[4:8])[0]
        used = hdr[8:0x8000]
        last = max(i for i, b in enumerate(used) if b)
        zero = bytes(block)
        for i in range(last + 1):
            out.write(f.read(block) if used[i] else zero)
        out.truncate(GC_DISC_SIZE)


def read_dol(f):
    f.seek(0x420)
    dol_off = struct.unpack(">I", f.read(4))[0]
    f.seek(dol_off)
    h = f.read(0x100)
    offs = struct.unpack(">18I", h[0:72])
    sizes = struct.unpack(">18I", h[0x90:0x90 + 72])
    f.seek(dol_off)
    return f.read(max(o + s for o, s in zip(offs, sizes)))


def verify(iso):
    with open(iso, "rb") as f:
        gid = f.read(6)
        if gid != GAME_ID:
            sys.exit(f"wrong game: {gid!r}, expected {GAME_ID!r} (Pokemon Colosseum USA)")
        sha = hashlib.sha1(read_dol(f)).hexdigest()
    if sha != MAIN_DOL_SHA1:
        sys.exit(f"main.dol sha1 {sha} does not match the supported release")
    print("ok: Pokemon Colosseum (USA), supported main.dol")


def extract(iso, out):
    with open(iso, "rb") as f:
        os.makedirs(f"{out}/sys", exist_ok=True)
        with open(f"{out}/sys/main.dol", "wb") as d:
            d.write(read_dol(f))
        f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", f.read(8))
        f.seek(fst_off)
        fst = f.read(fst_size)
        # System area, laid out the way Dolphin and ModernGekko expect.
        f.seek(0)
        sys_area = f.read(0x2440 + 0x20)
        app_size, app_trailer = struct.unpack(">II", sys_area[0x2454:0x245C])
        f.seek(0x2440)
        apploader = f.read(0x20 + app_size + app_trailer)
        for name, data in (("boot.bin", sys_area[:0x440]), ("bi2.bin", sys_area[0x440:0x2440]),
                           ("apploader.img", apploader), ("fst.bin", fst)):
            with open(f"{out}/sys/{name}", "wb") as d:
                d.write(data)
        n = struct.unpack(">I", fst[8:12])[0]
        strings = n * 12

        def name(i):
            o = struct.unpack(">I", fst[i * 12:i * 12 + 4])[0] & 0xFFFFFF
            s = fst[strings + o:]
            return s[:s.index(b"\0")].decode("latin1")

        stack = [(n, f"{out}/files")]
        os.makedirs(stack[0][1], exist_ok=True)
        count = 0
        for i in range(1, n):
            while i >= stack[-1][0]:
                stack.pop()
            w0, a, b = struct.unpack(">III", fst[i * 12:i * 12 + 12])
            path = os.path.join(stack[-1][1], name(i))
            if w0 >> 24:
                os.makedirs(path, exist_ok=True)
                stack.append((b, path))
            else:
                f.seek(a)
                with open(path, "wb") as o:
                    o.write(f.read(b))
                count += 1
    print(f"extracted main.dol and {count} files to {out}")


if __name__ == "__main__":
    cmds = {"ciso2iso": (ciso2iso, 2), "verify": (verify, 1), "extract": (extract, 2),
            "patch-dol": (patch_dol, 2), "patches-h": (patches_h, 1)}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds or len(sys.argv) - 2 != cmds[sys.argv[1]][1]:
        sys.exit(__doc__)
    cmds[sys.argv[1]][0](*sys.argv[2:])
