#!/usr/bin/env python3
"""Check/set IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE (0x0040) in PE headers.

Theory: PSEC containers force-rebase images that did not opt into ASLR,
per process -> cygwin fork's same-address-space requirement breaks.  Opting
in (DYNAMICBASE) makes the loader/section use one per-boot base per file,
shared by all processes -> fork again has identical layouts.

Usage:
  peflag.py check <pe-or-dir>...
  peflag.py set   <pe-or-dir>...   (refuses images without relocations)

Directories are walked recursively for *.exe/*.dll; non-PE32+ images are
skipped with a note (they cannot carry the flag).
"""
import os, struct, sys

DYNAMIC_BASE = 0x0040
RELOCS_STRIPPED = 0x0001

def read_pe(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if data[:2] != b"MZ":
        raise SystemExit("not MZ: %s" % path)
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew+4] != b"PE\0\0":
        raise SystemExit("not PE: %s" % path)
    coff = e_lfanew + 4
    characteristics = struct.unpack_from("<H", data, coff + 18)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        raise ValueError("not PE32+: %s" % path)
    dllchar_off = opt + 70
    dllchar = struct.unpack_from("<H", data, dllchar_off)[0]
    # Base relocation data directory (entry 5); starts at opt+0x70 for PE32+
    reloc_rva, reloc_sz = struct.unpack_from("<II", data, opt + 0x70 + 5 * 8)
    return data, dllchar_off, dllchar, characteristics, reloc_rva, reloc_sz

def expand(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for f in files:
                    if f.lower().endswith((".exe", ".dll")):
                        out.append(os.path.join(root, f))
        else:
            out.append(p)
    return out

for mode, paths in (("check", sys.argv[2:]),) if sys.argv[1] == "check" else (("set", sys.argv[2:]),):
    for p in expand(paths):
        try:
            data, off, dllchar, ch, rrva, rsz = read_pe(p)
        except ValueError as e:
            print("%s SKIPPED: %s" % (p, e))
            continue
        has_relocs = (ch & RELOCS_STRIPPED) == 0 and rrva != 0
        if mode == "check":
            print("%s dllchar=0x%04x dynamicbase=%s relocs_stripped=%s reloc_dir=0x%x/0x%x"
                  % (p, dllchar, bool(dllchar & DYNAMIC_BASE), bool(ch & RELOCS_STRIPPED), rrva, rsz))
        else:
            if dllchar & DYNAMIC_BASE:
                print("%s already set" % p)
                continue
            if not has_relocs:
                print("%s SKIPPED: no base relocations (cannot ASLR)" % p)
                continue
            struct.pack_into("<H", data, off, dllchar | DYNAMIC_BASE)
            with open(p, "wb") as f:
                f.write(data)
            print("%s SET (was 0x%04x, now 0x%04x)" % (p, dllchar, dllchar | DYNAMIC_BASE))
