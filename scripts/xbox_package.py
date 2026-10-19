#!/usr/bin/env python3
"""Lays out, packs and signs the Xbox (UWP) .msix.

    xbox_package.py <exe> <module.dll> <Sys dir> <signing dir> <out.msix> [version]

The signing dir holds ColosseumNative.pfx (made on first use, self-signed, CN=ColosseumNative
to match the manifest's Publisher) and ColosseumNative.cer for Device Portal. Packing uses
makemsix (microsoft/msix-packaging) from $MAKEMSIX; signing uses osslsigncode.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
UWP = HERE.parent / "app" / "uwp"
BG = (20, 22, 28)
ACCENT = (232, 176, 72)
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def tile(path, w, h, text):
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    # A simple ring mark plus the name; no game artwork.
    r = int(min(w, h) * 0.28)
    cx, cy = (w // 2, h // 2) if not text else (int(min(w, h) * 0.5), h // 2)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=ACCENT, width=max(2, r // 5))
    if text:
        x = cx + r + int(h * 0.12)
        room = w - x - int(h * 0.1)
        size = max(10, int(h * 0.17))
        font = ImageFont.truetype(FONT, size)
        while size > 8 and d.textlength(text, font=font) > room:
            size -= 1
            font = ImageFont.truetype(FONT, size)
        d.text((x, h // 2), text, fill=(235, 235, 240), font=font, anchor="lm")
    img.save(path)


def assets(dst):
    dst.mkdir(parents=True, exist_ok=True)
    tile(dst / "Square150x150Logo.png", 150, 150, "")
    tile(dst / "Square44x44Logo.png", 44, 44, "")
    tile(dst / "StoreLogo.png", 50, 50, "")
    tile(dst / "Wide310x150Logo.png", 310, 150, "Colosseum")
    tile(dst / "SplashScreen.png", 620, 300, "Colosseum Native")


def ensure_cert(signing):
    signing.mkdir(parents=True, exist_ok=True)
    pfx = signing / "ColosseumNative.pfx"
    if pfx.exists():
        return pfx
    key, crt = signing / "key.pem", signing / "cert.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:3072", "-sha256", "-days", "3650",
                    "-nodes", "-keyout", str(key), "-out", str(crt), "-subj", "/CN=ColosseumNative",
                    "-addext", "extendedKeyUsage=codeSigning",
                    "-addext", "basicConstraints=critical,CA:FALSE",
                    "-addext", "keyUsage=critical,digitalSignature"], check=True)
    subprocess.run(["openssl", "pkcs12", "-export", "-inkey", str(key), "-in", str(crt), "-out",
                    str(pfx), "-passout", "pass:"], check=True)
    subprocess.run(["openssl", "x509", "-in", str(crt), "-outform", "der", "-out",
                    str(signing / "ColosseumNative.cer")], check=True)
    return pfx


def main():
    exe, module, sysdir, signing, out = map(Path, sys.argv[1:6])
    version = sys.argv[6] if len(sys.argv) > 6 else "1.0.0.0"
    makemsix = os.environ.get("MAKEMSIX", "makemsix")
    with tempfile.TemporaryDirectory() as tmp:
        layout = Path(tmp) / "layout"
        layout.mkdir()
        shutil.copy2(exe, layout / "ColosseumXbox.exe")
        shutil.copy2(module, layout / "gGC6E01_recomp.dll")
        shutil.copytree(sysdir, layout / "Sys")
        assets(layout / "Assets")
        manifest = (UWP / "AppxManifest.xml").read_text().replace("@VERSION@", version)
        (layout / "AppxManifest.xml").write_text(manifest)
        unsigned = Path(tmp) / "unsigned.msix"
        subprocess.run([makemsix, "pack", "-d", str(layout), "-p", str(unsigned)], check=True)
        pfx = ensure_cert(signing)
        if out.exists():
            out.unlink()
        subprocess.run(["osslsigncode", "sign", "-pkcs12", str(pfx), "-pass", "", "-h", "sha256",
                        "-in", str(unsigned), "-out", str(out)], check=True)
    print(f"{out}: {out.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
