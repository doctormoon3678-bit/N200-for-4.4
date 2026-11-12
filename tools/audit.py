#!/usr/bin/env python3
"""Diagnose where n200 dies at startup.

Prints the exact System.loadLibrary() sequence out of classes.dex, then walks
each loaded library's DT_NEEDED closure and marks the ones that reach a private
platform ABI. The first marked entry in the sequence is where the launcher
activity's static initializer throws, because all of these loads happen in one
<clinit> -- so one failure kills the process before any window appears.

On the original APK this reports entry 10 ("dmb_cont") as the first casualty:

    10  dmb_cont          RISK  via libstagefright_dmb.so: 144 android:: symbols
                                via libskik.so: icu_51 / STLport internals

Usage:
    audit.py <apk-or-unpacked-dir> [--readelf PATH]
"""

import argparse
import pathlib
import shutil
import struct
import sys
import tempfile
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from verify_abi import (NDK_STABLE, PRIVATE_PATTERNS, dyn_syms,  # noqa: E402
                        find_readelf, needed)


# --------------------------------------------------------------------- dex
class Dex:
    def __init__(self, data: bytes):
        self.d = data
        self.str_n, self.str_off = struct.unpack_from("<II", data, 56)
        self.typ_n, self.typ_off = struct.unpack_from("<II", data, 64)
        self.mth_n, self.mth_off = struct.unpack_from("<II", data, 88)
        self.cls_n, self.cls_off = struct.unpack_from("<II", data, 96)

    def _uleb(self, o):
        r = s = 0
        while True:
            b = self.d[o]
            o += 1
            r |= (b & 0x7F) << s
            if not b & 0x80:
                return r, o
            s += 7

    def s(self, i):
        if i >= self.str_n:
            return None
        off = struct.unpack_from("<I", self.d, self.str_off + 4 * i)[0]
        _, p = self._uleb(off)
        return self.d[p:self.d.index(b"\x00", p)].decode("utf-8", "replace")

    def t(self, i):
        return self.s(struct.unpack_from("<I", self.d, self.typ_off + 4 * i)[0])

    def method(self, i):
        c, _, n = struct.unpack_from("<HHI", self.d, self.mth_off + 8 * i)
        return self.t(c), self.s(n)

    def code_owners(self):
        """code_item offset -> "Class->method"."""
        out = {}
        for i in range(self.cls_n):
            o = self.cls_off + 32 * i
            cidx, _, _, _, _, _, cdata, _ = struct.unpack_from("<IIIIIIII", self.d, o)
            if not cdata:
                continue
            cname = self.t(cidx)
            p = cdata
            sf, p = self._uleb(p)
            inf, p = self._uleb(p)
            dm, p = self._uleb(p)
            vm, p = self._uleb(p)
            for _ in range(sf + inf):
                _, p = self._uleb(p)
                _, p = self._uleb(p)
            for group in (dm, vm):
                midx = 0
                for _ in range(group):
                    di, p = self._uleb(p)
                    _, p = self._uleb(p)
                    co, p = self._uleb(p)
                    midx += di
                    if co:
                        _, mname = self.method(midx)
                        out[co] = f"{cname.strip('L;')}->{mname}"
        return out


def load_library_order(dex_bytes: bytes):
    """Return [(class->method, libname), ...] in bytecode order.

    Finds invoke-static of java.lang.System.loadLibrary (opcode 0x71, one
    argument => second byte 0x10) and walks backwards for the const-string that
    loaded the argument register. Scanning for the call pattern is used instead
    of a full disassembly because it needs no instruction-width table and cannot
    drift out of alignment.
    """
    dex = Dex(dex_bytes)
    wanted = [i for i in range(dex.mth_n)
              if dex.method(i) == ("Ljava/lang/System;", "loadLibrary")]
    if not wanted:
        return []
    owners = dex.code_owners()
    offs = sorted(owners)
    import bisect

    out = []
    for mi in wanted:
        pat = b"\x71\x10" + struct.pack("<H", mi)
        start = 0
        while True:
            k = dex_bytes.find(pat, start)
            if k < 0:
                break
            start = k + 1
            reg = dex_bytes[k + 4]
            lib = None
            for back in range(2, 80, 2):
                q = k - back
                if q < 0:
                    break
                if dex_bytes[q] == 0x1A and dex_bytes[q + 1] == reg:
                    lib = dex.s(struct.unpack_from("<H", dex_bytes, q + 2)[0])
                    break
                if dex_bytes[q] == 0x1B and dex_bytes[q + 1] == reg:
                    lib = dex.s(struct.unpack_from("<I", dex_bytes, q + 2)[0])
                    break
            j = bisect.bisect_right(offs, k) - 1
            out.append((k, owners[offs[j]] if j >= 0 else "?", lib))
    out.sort()
    return [(where, lib) for _, where, lib in out]


# --------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="APK file or unpacked APK directory")
    ap.add_argument("--readelf")
    args = ap.parse_args()

    readelf = find_readelf(args.readelf)
    target = pathlib.Path(args.target)
    tmp = None
    if target.is_file():
        tmp = tempfile.mkdtemp(prefix="n200audit.")
        with zipfile.ZipFile(target) as z:
            z.extractall(tmp)
        root = pathlib.Path(tmp)
    elif target.is_dir():
        root = target
    else:
        print(f"error: {target} not found", file=sys.stderr)
        return 2

    try:
        libs = {p.name: p for p in root.glob("lib/*/*.so")}
        if not libs:
            print(f"error: no lib/*/*.so inside {target}", file=sys.stderr)
            return 2

        bundled_defined: set[str] = set()
        sym = {}
        for name, p in libs.items():
            d, u = dyn_syms(readelf, p)
            sym[name] = (d, u)
            bundled_defined |= d
        dep = {name: needed(readelf, p) for name, p in libs.items()}

        def private_findings(name):
            """Private-ABI findings for one library, as {label: count}."""
            _, u = sym[name]
            found: dict[str, int] = {}
            for s in u - bundled_defined:
                for rx, label in PRIVATE_PATTERNS:
                    if rx.match(s):
                        found[label] = found.get(label, 0) + 1
                        break
            return found

        def closure(name, seen=None):
            seen = seen if seen is not None else set()
            if name in seen or name not in libs:
                return seen
            seen.add(name)
            for n in dep[name]:
                base = n.rsplit("/", 1)[-1]
                if base in libs:
                    closure(base, seen)
            return seen

        dexes = sorted(root.glob("classes*.dex"))
        if not dexes:
            print("error: no classes.dex", file=sys.stderr)
            return 2
        order = []
        for d in dexes:
            order += load_library_order(d.read_bytes())

        print("==> System.loadLibrary() calls in classes.dex, grouped by the")
        print("    method that makes them. Calls inside one method run in the")
        print("    order shown, and a static initializer runs them all before")
        print("    the class is usable -- so the first RISK line in a group is")
        print("    where that class, and the process, goes down.\n")

        groups: dict[str, list[str]] = {}
        for where, lib in order:
            groups.setdefault(where, []).append(lib)

        first_risk = None
        for where, members in groups.items():
            print(f"  {where}")
            for i, lib in enumerate(members, 1):
                soname = f"lib{lib}.so" if lib else "?"
                detail = []
                if soname in libs:
                    for member in sorted(closure(soname)):
                        f = private_findings(member)
                        if f:
                            for label, n in sorted(f.items()):
                                detail.append(f"via {member}: {n} x {label}")
                else:
                    detail.append("NOT PRESENT in the APK")
                tag = "RISK" if detail else "    "
                if detail and first_risk is None:
                    first_risk = (where, i, lib)
                print(f"    {i:3d}  {str(lib):<16s} {tag}  "
                      f"{detail[0] if detail else ''}")
                for extra in detail[1:]:
                    print(f"         {'':<16s}       {extra}")
            print()
        if first_risk:
            where, i, lib = first_risk
            print(f"RESULT: first failure expected at load #{i} ({lib}) in {where}.")
            return 1
        print("RESULT: no library in the load sequence reaches a private platform ABI.")
        return 0
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
