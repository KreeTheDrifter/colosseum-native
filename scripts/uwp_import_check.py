#!/usr/bin/env python3
"""Checks that a PE file only imports functions UWP apps may use.

    uwp_import_check.py <llvm-mingw dir> <file.exe|dll>...

The allowed set is what llvm-mingw's libwindowsapp.a and libucrtapp.a export (the umbrella
libraries for Windows Store apps), plus the package's own DLLs. Exits 1 and lists the
offending imports otherwise.
"""
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

PACKAGE_DLLS = {"ggc6e01_recomp.dll"}


def allowed(mingw):
    lib = Path(mingw) / "x86_64-w64-mingw32" / "lib"
    ok = defaultdict(set)
    for name in ("libwindowsapp.a", "libucrtapp.a"):
        out = subprocess.run(["llvm-nm", "-A", str(lib / name)], capture_output=True, text=True,
                             check=False).stdout
        if not out:
            out = subprocess.run(["llvm-nm-18", "-A", str(lib / name)], capture_output=True,
                                 text=True, check=True).stdout
        for line in out.splitlines():
            m = re.match(r".*?:([^:]+\.dll): [0-9a-f]+ T __imp_(\S+)", line, re.I)
            if m:
                ok[m.group(1).lower()].add(m.group(2))
    return ok


def imports(path):
    tool = "llvm-objdump"
    try:
        out = subprocess.run([tool, "-p", str(path)], capture_output=True, text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        out = subprocess.run(["llvm-objdump-18", "-p", str(path)], capture_output=True, text=True,
                             check=True).stdout
    result = defaultdict(set)
    dll = None
    for line in out.splitlines():
        m = re.match(r"\s*DLL Name: (\S+)", line)
        if m:
            dll = m.group(1).lower()
            continue
        m = re.match(r"\s+\d+\s+(\S+)$", line)
        if dll and m:
            result[dll].add(m.group(1))
        elif not line.strip():
            dll = None if line == "" and dll is None else dll
    return result


def main():
    ok = allowed(sys.argv[1])
    bad = []
    for path in sys.argv[2:]:
        for dll, names in sorted(imports(path).items()):
            if dll in PACKAGE_DLLS:
                continue
            for name in sorted(names):
                if name not in ok.get(dll, ()):
                    bad.append(f"{Path(path).name}: {dll}!{name}")
    if bad:
        print("Imports not available to UWP apps:")
        print("\n".join("  " + b for b in bad))
        return 1
    print("imports OK:", ", ".join(Path(p).name for p in sys.argv[2:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
