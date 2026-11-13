#!/usr/bin/env python3
"""Tests for tools/inject_needed.py, against a hand-built ELF32 ARM fixture.

A fixture is built here rather than taken from a real library because the APK
cannot be committed (it is a commercial app), and because nothing a modern
linker emits is usable either: lld packs .dynamic exactly, with no padding, so
there would be nothing to inject into. The app's own libraries, built with an
older NDK, carry four or five unused slots -- which is the whole reason the
technique works on them.

The fixture reproduces that shape: a DT_NULL followed by zeroed slots. The
checks then read the result back the way the dynamic linker does -- translate
DT_STRTAB through PT_LOAD, add the DT_NEEDED value, read a NUL-terminated
string -- rather than through the section headers inject_needed.py itself used,
so a mistake in that mapping cannot pass unnoticed.

Usage: tools/test_inject.py
"""

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from inject_needed import Err, inject, needed_names, sections  # noqa: E402

DT_NULL, DT_NEEDED, DT_STRTAB, DT_STRSZ = 0, 1, 5, 10

DYNSTR_OFF = 0x100
DYNAMIC_OFF = 0x200
IMAGE = 0x1000


def build_fixture(spare_slots: int, names=("libfoo.so", "libbar.so", "libc.so")):
    """A minimal but well-formed ELF32 ARM DYN object."""
    dynstr = b"\x00"
    offs = {}
    for n in names:
        offs[n] = len(dynstr)
        dynstr += n.encode() + b"\x00"

    entries = [(DT_NEEDED, offs[names[0]])]
    for n in names[1:]:
        entries.append((DT_NEEDED, offs[n]))
    entries += [(DT_STRTAB, DYNSTR_OFF), (DT_STRSZ, len(dynstr)), (DT_NULL, 0)]
    dyn = b"".join(struct.pack("<iI", t, v) for t, v in entries)
    dyn += b"\x00" * (8 * spare_slots)

    shstr = b"\x00.dynstr\x00.dynamic\x00.shstrtab\x00"
    shstr_off = DYNAMIC_OFF + len(dyn)
    shoff = (shstr_off + len(shstr) + 3) & ~3

    buf = bytearray(shoff + 4 * 40)
    buf[0:16] = b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 8
    struct.pack_into("<HHIIIIIHHHHHH", buf, 16,
                     3, 40, 1,            # e_type=DYN, e_machine=ARM, version
                     0, 52, shoff, 0x05000000,
                     52, 32, 2,           # ehsize, phentsize, phnum
                     40, 4, 3)            # shentsize, shnum, shstrndx
    # PT_LOAD over the whole image, then PT_DYNAMIC.
    struct.pack_into("<8I", buf, 52, 1, 0, 0, 0, IMAGE, IMAGE, 5, 0x1000)
    struct.pack_into("<8I", buf, 84, 2, DYNAMIC_OFF, DYNAMIC_OFF, DYNAMIC_OFF,
                     len(dyn), len(dyn), 6, 4)
    buf[DYNSTR_OFF:DYNSTR_OFF + len(dynstr)] = dynstr
    buf[DYNAMIC_OFF:DYNAMIC_OFF + len(dyn)] = dyn
    buf[shstr_off:shstr_off + len(shstr)] = shstr

    def sh(i, name_off, typ, flags, addr, off, size, align, entsize):
        struct.pack_into("<10I", buf, shoff + 40 * i,
                         name_off, typ, flags, addr, off, size, 0, 0,
                         align, entsize)

    sh(0, 0, 0, 0, 0, 0, 0, 0, 0)
    sh(1, 1, 3, 0x2, DYNSTR_OFF, DYNSTR_OFF, len(dynstr), 1, 0)       # .dynstr
    sh(2, 9, 6, 0x3, DYNAMIC_OFF, DYNAMIC_OFF, len(dyn), 4, 8)        # .dynamic
    sh(3, 18, 3, 0, 0, shstr_off, len(shstr), 1, 0)                   # .shstrtab
    return bytes(buf)


def linker_view(data: bytes):
    """Resolve DT_NEEDED the way bionic does: DT_STRTAB as a virtual address,
    translated through PT_LOAD, plus d_val. Deliberately does not consult the
    section headers."""
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    loads, dynseg = [], None
    for i in range(phnum):
        o = phoff + i * phentsize
        typ, off, va, _pa, filesz = struct.unpack_from("<5I", data, o)
        if typ == 1:
            loads.append((va, filesz, off))
        elif typ == 2:
            dynseg = (off, filesz)

    def v2o(v):
        for va, filesz, off in loads:
            if va <= v < va + filesz:
                return off + (v - va)
        raise AssertionError(f"vaddr 0x{v:x} is not inside any PT_LOAD")

    off, size = dynseg
    strtab = strsz = None
    needed = []
    for k in range(size // 8):
        tag, val = struct.unpack_from("<iI", data, off + 8 * k)
        if tag == DT_NULL:
            break
        if tag == DT_STRTAB:
            strtab = val
        elif tag == DT_STRSZ:
            strsz = val
        elif tag == DT_NEEDED:
            needed.append(val)
    out = []
    for val in needed:
        b = v2o(strtab + val)
        out.append((data[b:data.index(b"\x00", b)].decode(), val))
    return out, strtab, strsz


FAILED = 0


def ck(cond, what):
    global FAILED
    print(f"  {'ok   ' if cond else 'FAIL '} {what}")
    if not cond:
        FAILED += 1


def main() -> int:
    print("==> inject_needed.py")

    original = build_fixture(spare_slots=5)
    patched, msg = inject(original, "libn200fs.so")

    ck(len(patched) == len(original), "the file does not change size")

    view, strtab, strsz = linker_view(patched)
    names = [n for n, _ in view]
    ck(names == ["libn200fs.so", "libfoo.so", "libbar.so", "libc.so"],
       f"DT_NEEDED reads back as {names}")
    ck(names[0] == "libn200fs.so", "the new entry comes first, so before libc.so")
    ck(names[1:] == ["libfoo.so", "libbar.so", "libc.so"],
       "the existing entries keep their relative order")

    d_val = view[0][1]
    ck(d_val >= strsz - 13, "the name is stored past the original .dynstr")
    ck(strsz >= d_val + len("libn200fs.so") + 1,
       "DT_STRSZ was widened to cover it")

    # The string has to sit in .dynamic's padding, past its DT_NULL.
    secs = {s["name"]: s for s in sections(patched)}
    dyn = secs[".dynamic"]
    name_off = (strtab + d_val) - dyn["addr"]
    ck(0 < name_off < dyn["size"], "and that it lies inside .dynamic's padding")
    nulls = [k for k in range(dyn["size"] // 8)
             if struct.unpack_from("<iI", patched, dyn["off"] + 8 * k)[0] == DT_NULL]
    ck(nulls and 8 * nulls[0] < name_off,
       "with DT_NULL ahead of it, so the linker stops before reading it")

    again, msg2 = inject(patched, "libn200fs.so")
    ck(again == patched and "already" in msg2, "a second run changes nothing")

    # Everything outside .dynamic must be untouched.
    a = original[:DYNAMIC_OFF] + original[DYNAMIC_OFF + dyn["size"]:]
    b = patched[:DYNAMIC_OFF] + patched[DYNAMIC_OFF + dyn["size"]:]
    ck(a == b, "no byte outside .dynamic is modified")

    # Refusals.
    try:
        inject(build_fixture(spare_slots=0), "libn200fs.so")
        ck(False, "refuses a library with no padding to use")
    except Err as e:
        ck("no room" in str(e), f"refuses a library with no padding ({e})")

    tight = bytearray(build_fixture(spare_slots=1))
    try:
        inject(bytes(tight), "libn200fs.so")
        ck(False, "refuses when the name would not fit")
    except Err as e:
        ck("no room" in str(e), f"refuses when the name would not fit ({e})")

    dirty = bytearray(build_fixture(spare_slots=5))
    dirty[DYNAMIC_OFF + 8 * 7] = 0x41      # something lives in the padding
    try:
        inject(bytes(dirty), "libn200fs.so")
        ck(False, "refuses to overwrite non-zero padding")
    except Err as e:
        ck("not zero" in str(e), f"refuses to overwrite non-zero padding ({e})")

    # An independent parser must still accept the result.
    readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
    if readelf:
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "fixture.so"
            p.write_bytes(patched)
            r = subprocess.run([readelf, "-d", str(p)],
                               capture_output=True, text=True)
            ck(r.returncode == 0 and "libfoo.so" in r.stdout,
               "readelf still parses the patched .dynamic")
    else:
        print("  skip  readelf cross-check (not installed)")

    print()
    print(f"RESULT: {'PASS' if not FAILED else f'FAIL -- {FAILED} problem(s)'}")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
