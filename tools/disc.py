#!/usr/bin/env python3
"""Pokemon Colosseum (USA) disc tools: CISO -> ISO, identity check, extraction.

  disc.py ciso2iso <in.ciso> <out.iso>
  disc.py verify   <game.iso>
  disc.py extract  <game.iso> <outdir>     -> outdir/sys/main.dol, outdir/files/...
"""
import hashlib, os, struct, sys

GC_DISC_SIZE = 1_459_978_240
GAME_ID = b"GC6E01"
MAIN_DOL_SHA1 = "870e8b9693ca780782d80f22a6a4572d8ba9458f"  # USA rev 0


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
    cmds = {"ciso2iso": (ciso2iso, 2), "verify": (verify, 1), "extract": (extract, 2)}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds or len(sys.argv) - 2 != cmds[sys.argv[1]][1]:
        sys.exit(__doc__)
    cmds[sys.argv[1]][0](*sys.argv[2:])
