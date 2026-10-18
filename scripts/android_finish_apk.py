#!/usr/bin/env python3
"""Finish the Colosseum Native Android APK: add the recompiled game code and sign it.

    android_finish_apk.py <unsigned.apk> <libgGC6E01_recomp.so> <key dir> <out.apk>

The CI-built APK (android-build branch) has no game code. This adds the arm64 module
(built privately from the player's disc, never published) as lib/arm64-v8a/, then signs the
APK with APK Signature Scheme v2 (RSA 2048 / SHA-256). The key is created in <key dir> on
first use and reused after, so updates install over each other. Needs: python3-cryptography.
"""
import os
import struct
import sys
import zlib
from datetime import datetime, timedelta, timezone
from hashlib import sha256

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import NameOID

MODULE_ENTRY = "lib/arm64-v8a/libgGC6E01_recomp.so"


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def find_eocd(data):
    at = data.rfind(b"PK\x05\x06", max(0, len(data) - 65557))
    if at < 0:
        sys.exit("not a zip file")
    return at


def read_entries(data):
    """Central directory entries: (name, central header bytes, local record bytes)."""
    eocd = find_eocd(data)
    count = u16(data, eocd + 10)
    cd_offset = u32(data, eocd + 16)
    if data[cd_offset - 16:cd_offset] == b"APK Sig Block 42":
        pass  # existing signature: dropped below (entries are rebuilt)
    entries = []
    p = cd_offset
    for _ in range(count):
        if data[p:p + 4] != b"PK\x01\x02":
            sys.exit("bad central directory")
        flags = u16(data, p + 8)
        method = u16(data, p + 10)
        csize = u32(data, p + 20)
        name_len, extra_len, comment_len = u16(data, p + 28), u16(data, p + 30), u16(data, p + 32)
        local = u32(data, p + 42)
        name = data[p + 46:p + 46 + name_len].decode("utf-8")
        central = bytearray(data[p:p + 46 + name_len + extra_len + comment_len])
        # Local record: header + data (+ data descriptor).
        lname, lextra = u16(data, local + 26), u16(data, local + 28)
        start = local + 30 + lname + lextra
        end = start + csize
        if flags & 8:
            end += 16 if data[end:end + 4] == b"PK\x07\x08" else 12
        header = bytearray(data[local:local + 30 + lname])  # extra dropped, re-added for alignment
        struct.pack_into("<H", header, 28, 0)
        entries.append({"name": name, "central": central, "header": header,
                        "payload": data[start:end], "method": method})
        p += len(central)
    return entries


def make_entry(name, content):
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    payload = comp.compress(content) + comp.flush()
    crc = zlib.crc32(content) & 0xFFFFFFFF
    raw = name.encode()
    dos_time, dos_date = 0, (1 << 5) | 1  # 1980-01-01, stable output
    header = bytearray(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0x0800, 8, dos_time, dos_date,
                                   crc, len(payload), len(content), len(raw), 0) + raw)
    central = bytearray(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0x0800, 8, dos_time,
                                    dos_date, crc, len(payload), len(content), len(raw), 0, 0, 0,
                                    0, 0, 0) + raw)
    return {"name": name, "central": central, "header": header, "payload": payload, "method": 8}


def write_zip(entries):
    """Zip bytes with stored entries aligned (4 bytes, 16 KiB for .so); returns (data, cd_offset)."""
    out = bytearray()
    for e in entries:
        offset = len(out)
        header = e["header"]
        extra = b""
        if e["method"] == 0:
            align = 16384 if e["name"].endswith(".so") else 4
            data_start = offset + len(header)
            pad = (-data_start) % align
            if pad:
                # Pad with a zero-filled extra field (ID 0xD935, like zipalign) when it fits.
                if pad < 4:
                    pad += align
                extra = struct.pack("<HH", 0xD935, pad - 4) + b"\0" * (pad - 4)
        header = bytearray(header)
        struct.pack_into("<H", header, 28, len(extra))
        out += header + extra + e["payload"]
        central = e["central"]
        struct.pack_into("<I", central, 42, offset)
        e["central"] = central
    cd_offset = len(out)
    for e in entries:
        out += e["central"]
    cd_size = len(out) - cd_offset
    out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), cd_size, cd_offset, 0)
    return bytes(out), cd_offset


def load_key(key_dir):
    os.makedirs(key_dir, exist_ok=True)
    key_path = os.path.join(key_dir, "colosseum-native-key.pem")
    cert_path = os.path.join(key_dir, "colosseum-native-cert.der")
    if os.path.exists(key_path) and os.path.exists(cert_path):
        key = serialization.load_pem_private_key(open(key_path, "rb").read(), None)
        return key, open(cert_path, "rb").read()
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "Colosseum Native (private test)")])
    now = datetime.now(timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(days=1)).not_valid_after(now + timedelta(days=365 * 30))
            .sign(key, hashes.SHA256()))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    os.chmod(key_path, 0o600)
    der = cert.public_bytes(serialization.Encoding.DER)
    with open(cert_path, "wb") as f:
        f.write(der)
    return key, der


def lp(b):
    return struct.pack("<I", len(b)) + b


def chunked_digest(sections):
    chunks = []
    for section in sections:
        for i in range(0, len(section), 1 << 20):
            piece = section[i:i + (1 << 20)]
            chunks.append(sha256(b"\xa5" + struct.pack("<I", len(piece)) + piece).digest())
    return sha256(b"\x5a" + struct.pack("<I", len(chunks)) + b"".join(chunks)).digest()


def sign_v2(zip_data, cd_offset, key, cert_der):
    eocd = find_eocd(zip_data)
    digest = chunked_digest([zip_data[:cd_offset], zip_data[cd_offset:eocd], zip_data[eocd:]])
    algo = 0x0103  # RSASSA-PKCS1-v1_5 with SHA2-256
    digests = lp(struct.pack("<I", algo) + lp(digest))
    signed_data = lp(digests) + lp(lp(cert_der)) + lp(b"")
    signature = key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    public_key = key.public_key().public_bytes(serialization.Encoding.DER,
                                               serialization.PublicFormat.SubjectPublicKeyInfo)
    signer = lp(signed_data) + lp(lp(struct.pack("<I", algo) + lp(signature))) + lp(public_key)
    value = lp(lp(signer))
    pair = struct.pack("<Q", 4 + len(value)) + struct.pack("<I", 0x7109871A) + value
    size = len(pair) + 8 + 16
    block = struct.pack("<Q", size) + pair + struct.pack("<Q", size) + b"APK Sig Block 42"
    eocd_bytes = bytearray(zip_data[eocd:])
    struct.pack_into("<I", eocd_bytes, 16, cd_offset + len(block))
    return zip_data[:cd_offset] + block + zip_data[cd_offset:eocd] + bytes(eocd_bytes)


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    apk, module, key_dir, out = sys.argv[1:]
    data = open(apk, "rb").read()
    entries = [e for e in read_entries(data)
               if not e["name"].startswith("META-INF/") or not e["name"].endswith(
                   (".SF", ".RSA", ".DSA", ".EC", "MANIFEST.MF"))]
    entries = [e for e in entries if e["name"] != MODULE_ENTRY]
    entries.append(make_entry(MODULE_ENTRY, open(module, "rb").read()))
    zip_data, cd_offset = write_zip(entries)
    key, cert = load_key(key_dir)
    with open(out, "wb") as f:
        f.write(sign_v2(zip_data, cd_offset, key, cert))
    print(f"{out}: {os.path.getsize(out) / 1e6:.1f} MB, signed (v2), cert sha256 "
          f"{sha256(cert).hexdigest()[:16]}…")


if __name__ == "__main__":
    main()
