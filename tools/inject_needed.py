#!/usr/bin/env python3
"""Add a DT_NEEDED entry to an ARM shared library, in place, without resizing.

WHY
---
The pre-N Android dynamic linker resolves a library's relocations by searching,
in order: the main executable, LD_PRELOAD, then that library's own DT_NEEDED
list -- and nothing else. So a symbol is interposed by being defined in a
library that appears in the DT_NEEDED list *before* the one that normally
defines it. Making every app library list libn200fs.so ahead of libc.so is what
binds their fopen/open/stat calls to the redirector instead of to libc.

HOW IT FITS WITHOUT GROWING THE FILE
------------------------------------
Two things are needed for a new DT_NEEDED: a free slot in .dynamic, and the
library name as a NUL-terminated string that DT_STRTAB + d_val points at.

Both come out of .dynamic itself. The section is padded -- every library in
this APK carries 4 or 5 unused 8-byte slots of zeroes past its DT_NULL
terminator. One becomes the new entry, DT_NULL moves up one, and the name
string is written into the slots after it. The linker stops reading at DT_NULL
and never sees the string; the string is addressed by its own virtual address,
so it does not matter that it lies outside .dynstr:

    d_val = (.dynamic vaddr + 8*(slots_used+1)) - (.dynstr vaddr)

Android 4.4's linker computes `si->strtab + d_val` directly and never looks at
DT_STRSZ, and both sections are mapped by PT_LOADs of the same object, so the
sum lands exactly on the string. Nothing moves, so every section header,
relocation, symbol and program header stays valid.

DT_STRSZ is widened to cover the new string anyway. Nothing on 4.4 reads it,
but a linker that does bounds-check a string index against it -- bionic grew
exactly that check in Android 7 -- then accepts the entry instead of aborting.
It costs two bytes of a field nobody else uses and removes the one assumption
this tool would otherwise be making about the runtime.

ORDER IS PRESERVED
------------------
The new entry is made the *first* DT_NEEDED, since libc.so has to come after
it, and in libpowertts.so libc.so is already first. The existing entries are
shifted down one slot each, so their order relative to each other is unchanged
and no other library's symbol resolution shifts.

Usage:
    inject_needed.py <file.so|dir> [--name libn200fs.so] [--verify] [--quiet]
"""

import argparse
import pathlib
import struct
import sys

DT_NULL, DT_NEEDED, DT_STRTAB, DT_STRSZ = 0, 1, 5, 10


class Err(Exception):
    pass


def sections(data: bytes):
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise Err("not an ELF32 file")
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 46)
    out = []
    for i in range(shnum):
        o = shoff + i * shentsize
        name, typ, flags, addr, off, size = struct.unpack_from("<6I", data, o)
        out.append(dict(name_off=name, type=typ, addr=addr, off=off, size=size))
    strtab = out[shstrndx]
    for s in out:
        b = strtab["off"] + s["name_off"]
        s["name"] = data[b:data.index(b"\x00", b)].decode()
    return out


def dyn_entries(data: bytes, dyn: dict):
    return [struct.unpack_from("<iI", data, dyn["off"] + 8 * k)
            for k in range(dyn["size"] // 8)]


def needed_names(data: bytes, dyn: dict, dynstr: dict):
    names = []
    for tag, val in dyn_entries(data, dyn):
        if tag == DT_NULL:
            break
        if tag == DT_NEEDED:
            b = dynstr["off"] + val
            # The name may live past .dynstr (that is what this tool does), so
            # read from the mapped image by virtual address instead.
            names.append(read_at_vaddr(data, dynstr["addr"] + val))
    return names


def read_at_vaddr(data: bytes, vaddr: int) -> str:
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    for i in range(phnum):
        o = phoff + i * phentsize
        typ, off, va, pa, filesz = struct.unpack_from("<5I", data, o)
        if typ == 1 and va <= vaddr < va + filesz:
            b = off + (vaddr - va)
            return data[b:data.index(b"\x00", b)].decode("utf-8", "replace")
    return f"<unmapped@0x{vaddr:x}>"


def inject(data: bytes, libname: str) -> tuple[bytes, str]:
    """Returns (new_data, message). Raises Err if it cannot be done."""
    secs = sections(data)
    by = {s["name"]: s for s in secs}
    if ".dynamic" not in by or ".dynstr" not in by:
        raise Err("no .dynamic/.dynstr (not a dynamically linked object)")
    dyn, dynstr = by[".dynamic"], by[".dynstr"]

    entries = dyn_entries(data, dyn)
    try:
        null_at = next(k for k, (t, v) in enumerate(entries) if t == DT_NULL)
    except StopIteration:
        raise Err(".dynamic has no DT_NULL terminator")
    used = null_at + 1                      # slots in use, DT_NULL included

    if libname in needed_names(data, dyn, dynstr):
        return data, f"already lists {libname}"

    name_bytes = libname.encode() + b"\x00"
    str_slot = used + 1                     # first slot past the new DT_NULL
    str_off = 8 * str_slot
    if str_off + len(name_bytes) > dyn["size"]:
        raise Err(f".dynamic has no room: needs {str_off + len(name_bytes)} "
                  f"bytes, section is {dyn['size']}")

    tail = data[dyn["off"] + 8 * used: dyn["off"] + dyn["size"]]
    if any(tail):
        raise Err("the padding past DT_NULL in .dynamic is not zero; "
                  "refusing to overwrite it")

    str_vaddr = dyn["addr"] + str_off
    if str_vaddr <= dynstr["addr"]:
        raise Err(".dynamic lies before .dynstr, so the name offset would be "
                  "negative")
    d_val = str_vaddr - dynstr["addr"]

    buf = bytearray(data)

    def put(slot, tag, val):
        struct.pack_into("<iI", buf, dyn["off"] + 8 * slot, tag, val)

    # Shift every DT_NEEDED value one slot along its own sequence, so the new
    # one can take the first position without disturbing the relative order of
    # the rest. The displaced last value lands in the old DT_NULL slot.
    slots = [k for k, (t, v) in enumerate(entries[:null_at]) if t == DT_NEEDED]
    values = [entries[k][1] for k in slots]
    chain = slots + [null_at]
    for dst, val in zip(chain[1:], values):
        put(dst, DT_NEEDED, val)
    put(chain[0], DT_NEEDED, d_val)
    put(used, DT_NULL, 0)
    buf[dyn["off"] + str_off: dyn["off"] + str_off + len(name_bytes)] = name_bytes

    # Widen DT_STRSZ so the new index is in bounds for any linker that checks.
    need = d_val + len(name_bytes)
    for k, (tag, val) in enumerate(entries[:null_at]):
        if tag == DT_STRSZ and val < need:
            # entries[] was read before the shift, so find where DT_STRSZ
            # actually sits now -- the shift only moved DT_NEEDED values.
            put(k, DT_STRSZ, need)
            break

    if len(buf) != len(data):
        raise Err("internal: file size changed")
    return bytes(buf), (f"{libname} added as DT_NEEDED #1 "
                        f"(slot {chain[0]}, name at .dynamic+{str_off})")


# The entry points libn200fs.so interposes. A library that imports none of
# them has no file call to redirect, so adding the dependency would only be a
# way to break it.
INTERPOSED = ("fopen open stat access opendir mkdir rmdir unlink remove "
              "rename utimes statfs system").split()


def _dynsym_names(data: bytes, defined: bool) -> set[str]:
    secs = sections(data)
    by = {s["name"]: s for s in secs}
    if ".dynsym" not in by or ".dynstr" not in by:
        return set()
    sym, strt = by[".dynsym"], by[".dynstr"]
    out = set()
    for i in range(sym["size"] // 16):
        o = sym["off"] + 16 * i
        st_name, _, _, info, _, st_shndx = struct.unpack_from("<IIIBBH", data, o)
        if st_name == 0:
            continue
        if defined:
            if st_shndx == 0 or (info >> 4) not in (1, 2):   # GLOBAL, WEAK
                continue
        elif st_shndx != 0:
            continue
        b = strt["off"] + st_name
        out.add(data[b:data.index(b"\x00", b)].decode("utf-8", "replace"))
    return out


def undefined_symbols(data: bytes) -> set[str]:
    return _dynsym_names(data, defined=False)


def defined_symbols(data: bytes) -> set[str]:
    return _dynsym_names(data, defined=True)


def report(path: pathlib.Path, libname: str) -> bool:
    data = path.read_bytes()
    secs = sections(data)
    by = {s["name"]: s for s in secs}
    names = needed_names(data, by[".dynamic"], by[".dynstr"])
    if libname not in names:
        print(f"  FAIL  {path.name}: does not list {libname}")
        return False
    i, j = names.index(libname), (names.index("libc.so")
                                  if "libc.so" in names else len(names))
    if i >= j:
        print(f"  FAIL  {path.name}: {libname} at #{i} is not before "
              f"libc.so at #{j}")
        return False
    # Every library in this APK is built -Bsymbolic, so the linker looks inside
    # the library itself before it looks at DT_NEEDED at all. A library that
    # defined one of these names would keep calling its own, whatever position
    # the redirector holds.
    own = defined_symbols(data) & set(INTERPOSED)
    if own:
        print(f"  FAIL  {path.name}: defines {sorted(own)} itself, which "
              f"-Bsymbolic resolves ahead of {libname}")
        return False
    print(f"  ok    {path.name}: {libname} #{i}, libc.so #{j}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help=".so file, or a directory of them")
    ap.add_argument("--name", default="libn200fs.so",
                    help="library to add (default libn200fs.so)")
    ap.add_argument("--verify", action="store_true",
                    help="check only: is the entry present and ahead of libc?")
    ap.add_argument("--skip", action="append", default=[], metavar="NAME",
                    help="basename to leave alone; repeatable")
    ap.add_argument("--all", action="store_true",
                    help="patch every library, not just the ones that import "
                         "an interposed entry point")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    t = pathlib.Path(args.target)
    if t.is_dir():
        files = sorted(p for p in t.glob("*.so"))
    elif t.is_file():
        files = [t]
    else:
        print(f"error: no such file or directory: {t}", file=sys.stderr)
        return 1
    files = [p for p in files if p.name not in args.skip and p.name != args.name]
    if not files:
        print(f"error: nothing to do under {t}", file=sys.stderr)
        return 1

    if not args.all:
        wanted = set(INTERPOSED)
        keep, drop = [], []
        for p in files:
            (keep if undefined_symbols(p.read_bytes()) & wanted else drop).append(p)
        if drop and not args.quiet:
            print(f"    skipping {len(drop)} library(ies) that call none of the "
                  f"interposed entry points")
        files = keep
        if not files:
            print("error: no library imports an interposed entry point; is this "
                  "the right directory?", file=sys.stderr)
            return 1

    if args.verify:
        bad = [p for p in files if not report(p, args.name)]
        return 1 if bad else 0

    changed = skipped = 0
    for p in files:
        try:
            data = p.read_bytes()
            new, msg = inject(data, args.name)
        except Err as e:
            print(f"error: {p.name}: {e}", file=sys.stderr)
            return 1
        if new == data:
            skipped += 1
            if not args.quiet:
                print(f"    --   {p.name}: {msg}")
            continue
        if not args.dry_run:
            p.write_bytes(new)
        changed += 1
        if not args.quiet:
            verb = "would patch" if args.dry_run else "patched"
            print(f"    {verb} {p.name}: {msg}")
    if not args.quiet:
        print(f"    ---- {changed} patched, {skipped} already done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
