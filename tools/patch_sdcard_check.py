#!/usr/bin/env python3
"""Make the app believe the SD card is mounted.

THE PROBLEM
-----------
The navigation engine will not start until the Java side tells it the card is
there. LocalServiceHMI answers that question:

    void RequestSDCardCheck() {
        if (preSDcardState)
            SetMessgeEvent(0, 1, PATH_SDCARD);   // "card is mounted"
    }

and preSDcardState is set exactly once, in LocalServiceHMI.onCreate:

    PATH_SDCARD = "mnt/extSdCard";              // from <init>
    if (Environment.getStorageState(new File(PATH_SDCARD)).equals("mounted"))
        preSDcardState = true;

Environment.getStorageState() resolves that against the list of mounted
volumes. On a phone the card is /storage/extSdCard or similar and
/mnt/extSdCard is not a volume at all, so it returns "unknown", the flag stays
false, the event is never sent, and the engine waits forever -- the app sits on
its "preparing navigation" screen and never moves.

Nothing else can set the flag later. sendEventMediaMount() is the only other
writer, and it only runs for a MEDIA_MOUNTED broadcast whose path equals
PATH_SDCARD, which is the same string that does not match anything here.

This is a Java-side check, so libn200fs.so cannot help: it interposes libc, and
Environment.getStorageState goes to the mount service over Binder without
touching a file.

WHY NOT JUST REWRITE THE PATH
-----------------------------
Because dex string_ids are sorted and dexopt verifies that at install time. The
replacement would have to sort between "mknod" and
"mnt/extSdCard/Snapshot/screen", which rules out every path that would actually
work: "/sdcard", "/mnt/sdcard" and "/storage/..." all sort far away from "mnt".

THE PATCH
---------
Replace the `if-eqz` that guards the assignment with two `nop`s, so the
assignment always runs:

    0050  invoke-virtual   {v6, v10} String.equals
    0053  move-result      v10
    0054  if-eqz           v10, +4        <-- becomes nop nop
    0056  iput-boolean     v13, preSDcardState

Two code units in, two out, so nothing moves: no string, no offset, no
checksum arithmetic beyond the header, and the sorted string table is not
touched at all.

The instruction is found by matching the surrounding pattern rather than by a
baked-in offset, and every assumption is checked before a byte is written, so
a different build of the APK is refused instead of corrupted.

Usage:
    patch_sdcard_check.py <unpacked-apk-dir> [--dry-run] [--quiet]
"""

import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from patch_paths import (fix_dex_header, verify_dex_order,  # noqa: E402
                         verify_dex_strings)

OP_IF_EQZ = 0x38
OP_IPUT_BOOLEAN = 0x5C
OP_NOP = 0x00

# Dalvik instruction width in 16-bit code units, by opcode. Only what is
# needed to walk a method body.
WIDTH = {}
for _o in range(0x100):
    WIDTH[_o] = 1
for _o, _w in {
    0x02: 2, 0x03: 3, 0x05: 2, 0x06: 3, 0x08: 2, 0x09: 3, 0x13: 2, 0x14: 3,
    0x15: 2, 0x16: 2, 0x17: 3, 0x18: 5, 0x19: 2, 0x1a: 2, 0x1b: 3, 0x1c: 2,
    0x1f: 2, 0x20: 2, 0x22: 2, 0x23: 2, 0x24: 3, 0x25: 3, 0x26: 3, 0x29: 2,
    0x2a: 3, 0x2b: 3, 0x2c: 3, 0x2d: 2, 0x2e: 2, 0x2f: 2, 0x30: 2, 0x31: 2,
    0x32: 2, 0x33: 2, 0x34: 2, 0x35: 2, 0x36: 2, 0x37: 2, 0x38: 2, 0x39: 2,
    0x3a: 2, 0x3b: 2, 0x3c: 2, 0x3d: 2,
}.items():
    WIDTH[_o] = _w
for _o in range(0x44, 0x52):
    WIDTH[_o] = 2
for _o in range(0x52, 0x6e):
    WIDTH[_o] = 2
for _o in range(0x6e, 0x79):
    WIDTH[_o] = 3
for _o in range(0x90, 0xb0):
    WIDTH[_o] = 2
for _o in range(0xd0, 0xe3):
    WIDTH[_o] = 2


class Err(Exception):
    pass


def uleb(d, o):
    r = s = n = 0
    while True:
        b = d[o + n]
        n += 1
        r |= (b & 0x7F) << s
        if not b & 0x80:
            return r, o + n
        s += 7


class Dex:
    def __init__(self, data):
        self.d = data
        (self.str_n, self.str_off, self.type_n, self.type_off,
         self.proto_n, self.proto_off, self.fld_n, self.fld_off,
         self.mth_n, self.mth_off, self.cls_n, self.cls_off,
         _dsz, _doff) = struct.unpack_from("<14I", data, 56)

    def string(self, i):
        off = struct.unpack_from("<I", self.d, self.str_off + 4 * i)[0]
        _, p = uleb(self.d, off)
        return self.d[p:self.d.index(b"\x00", p)].decode("utf-8", "replace")

    def type_name(self, i):
        return self.string(struct.unpack_from("<I", self.d, self.type_off + 4 * i)[0])

    def field(self, i):
        c, t, n = struct.unpack_from("<HHI", self.d, self.fld_off + 8 * i)
        return self.type_name(c), self.string(n)

    def method_name(self, i):
        c, _p, n = struct.unpack_from("<HHI", self.d, self.mth_off + 8 * i)
        return self.type_name(c), self.string(n)

    def find_method_code(self, cls, name):
        """Return the code_item offset of cls->name, or None."""
        for i in range(self.cls_n):
            cidx, _f, _s, _ifc, _src, _ann, cdata, _sv = struct.unpack_from(
                "<8I", self.d, self.cls_off + 32 * i)
            if self.type_name(cidx) != cls or not cdata:
                continue
            o = cdata
            sf, o = uleb(self.d, o)
            inf, o = uleb(self.d, o)
            dm, o = uleb(self.d, o)
            vm, o = uleb(self.d, o)
            for _ in range(sf + inf):
                _, o = uleb(self.d, o)
                _, o = uleb(self.d, o)
            for grp in (dm, vm):
                idx = 0
                for _ in range(grp):
                    di, o = uleb(self.d, o)
                    idx += di
                    _, o = uleb(self.d, o)
                    co, o = uleb(self.d, o)
                    if self.method_name(idx)[1] == name:
                        return co
        return None

    def walk(self, code_off):
        """yield (pc, opcode) over a method body."""
        regs, ins, outs, tries, dbg, isz = struct.unpack_from(
            "<HHHHII", self.d, code_off)
        base = code_off + 16
        p = 0
        while p < isz:
            unit = struct.unpack_from("<H", self.d, base + 2 * p)[0]
            op = unit & 0xFF
            w = WIDTH[op]
            if op == OP_NOP and (unit >> 8) in (1, 2, 3):
                kind = unit >> 8
                if kind == 1:
                    size = struct.unpack_from("<H", self.d, base + 2 * p + 2)[0]
                    w = size * 2 + 4
                elif kind == 2:
                    size = struct.unpack_from("<H", self.d, base + 2 * p + 2)[0]
                    w = size * 4 + 2
                else:
                    esz, sz = struct.unpack_from("<HI", self.d, base + 2 * p + 2)
                    w = (esz * sz + 1) // 2 + 4
            yield p, op, base
            p += w


def patch(data: bytes, quiet=False) -> tuple[bytes, str]:
    dex = Dex(data)
    CLS = "Lcom/npnd/n200/serv/LocalServiceHMI;"
    code = dex.find_method_code(CLS, "onCreate")
    if code is None:
        raise Err(f"{CLS}->onCreate not found")

    # Find: if-eqz vX, +4  /  iput-boolean vY, preSDcardState
    base = None
    target = None
    prev = None
    for p, op, b in dex.walk(code):
        base = b
        if op == OP_IPUT_BOOLEAN:
            fidx = struct.unpack_from("<H", data, b + 2 * p + 2)[0]
            cls, fname = dex.field(fidx)
            if cls == CLS and fname == "preSDcardState":
                if prev is None or prev[1] != OP_IF_EQZ:
                    raise Err(f"the assignment to preSDcardState at {p:#06x} is "
                              f"not guarded by if-eqz (found opcode "
                              f"{prev[1]:#04x} before it)")
                off = struct.unpack_from("<h", data, b + 2 * prev[0] + 2)[0]
                # The branch must jump past the assignment, i.e. skip it.
                if prev[0] + off <= p:
                    raise Err(f"if-eqz at {prev[0]:#06x} branches backwards to "
                              f"{prev[0] + off:#06x}; refusing to touch it")
                target = prev[0]
                break
        prev = (p, op)

    if target is None:
        raise Err("no assignment to preSDcardState found in onCreate")

    buf = bytearray(data)
    at = base + 2 * target
    if struct.unpack_from("<H", buf, at)[0] & 0xFF != OP_IF_EQZ:
        raise Err("internal: not an if-eqz after all")
    struct.pack_into("<HH", buf, at, 0, 0)          # nop nop
    out = fix_dex_header(bytes(buf))

    problems = verify_dex_strings(out) + verify_dex_order(out)
    if problems:
        raise Err("the patched dex would be rejected at install time: "
                  + "; ".join(problems[:3]))
    if len(out) != len(data):
        raise Err("internal: dex changed size")
    return out, (f"if-eqz at {target:#06x} in LocalServiceHMI.onCreate -> nop nop; "
                 f"preSDcardState is now always true")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="directory holding the unpacked APK")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = pathlib.Path(args.root)
    dexes = sorted(root.glob("classes*.dex"))
    if not dexes:
        print(f"error: no classes.dex under {root}", file=sys.stderr)
        return 1

    done = 0
    for dp in dexes:
        data = dp.read_bytes()
        try:
            out, msg = patch(data, args.quiet)
        except Err as e:
            if len(dexes) > 1:
                continue
            print(f"error: {dp.name}: {e}", file=sys.stderr)
            return 1
        if not args.dry_run:
            dp.write_bytes(out)
        done += 1
        if not args.quiet:
            verb = "would patch" if args.dry_run else "patched"
            print(f"    {verb} {dp.name}: {msg}")
    if not done:
        print("error: LocalServiceHMI.onCreate not found in any dex",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
