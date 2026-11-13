#!/usr/bin/env python3
"""Rewrite the hard-coded data root inside an unpacked n200 APK.

The app hard-codes /mnt/extSdCard as its data root, in both the native
libraries and classes.dex:

    /mnt/extSdCard///Navkingdom/Navi/CONF/naviConfig_N200.txt
    /mnt/extSdCard///Navkingdom/Persistence/...        <- written at runtime
    /mnt/extSdCard///Navkingdom/Navi/DB_HMI/Resources

On a Galaxy Note 4 running KitKat the secondary SD card is mounted at
/storage/extSdCard and /mnt/extSdCard does not exist at all. Worse, from
KitKat onwards WRITE_EXTERNAL_STORAGE no longer lets a third-party app write
anywhere on secondary storage except Android/data/<package>/, so the
Persistence/*.bak writes would fail even if the directory were there.

So we repoint the data root at primary storage, which the app can read and
write with the permission it already declares.

The replacement is EQUAL LENGTH by default (/mnt/extSdCard and /sdcard/n200dt
are both 14 bytes). That matters: an equal-length, in-place byte substitution
shifts no offsets, so every ELF section header, relocation, symbol table entry
and string length stays valid, and the dex string table's ULEB128 length
prefixes stay correct. Nothing needs to be relinked.

A shorter replacement is possible with --allow-shorter, which left-shifts the
remainder of each NUL-terminated string and pads the tail with NULs. That is
safe only if no code holds a precomputed length for those strings or points
into the middle of one, which we cannot prove, so it is off by default.

For classes.dex the header checksum (adler32) and signature (SHA-1) are
recomputed, otherwise the runtime rejects the file.

Usage:
    patch_paths.py <unpacked-apk-dir> [--map OLD=NEW] [--allow-shorter]
                                      [--dry-run] [--quiet]
"""

import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

# The absolute form must be substituted first: once "/mnt/extSdCard" is gone,
# only the genuinely relative occurrences are left for the second pair. The
# relative form appears in SWUpdater's shell commands, e.g.
# "cp -r mnt/extSdCard/TempSW/". Both pairs are equal length (14 and 13 bytes).
DEFAULT_MAP = [
    ("/mnt/extSdCard", "/sdcard/n200dt"),
    ("mnt/extSdCard", "sdcard/n200dt"),
]


def patch_bytes_equal(data: bytes, old: bytes, new: bytes) -> tuple[bytes, int]:
    """Plain equal-length substitution. No offsets move."""
    n = data.count(old)
    if n:
        data = data.replace(old, new)
    return data, n


def patch_bytes_shorter(data: bytes, old: bytes, new: bytes) -> tuple[bytes, int]:
    """Shorter replacement: left-shift the rest of each NUL-terminated string
    and NUL-pad the tail, so the containing byte range keeps its size."""
    buf = bytearray(data)
    gap = len(old) - len(new)
    count = 0
    pos = 0
    while True:
        i = buf.find(old, pos)
        if i < 0:
            break
        end = buf.find(b"\x00", i)
        if end < 0:
            # No terminator ahead; refuse to touch this one.
            pos = i + len(old)
            continue
        tail = bytes(buf[i + len(old):end])
        buf[i:end] = new + tail + b"\x00" * gap
        count += 1
        # Resume just after the replacement, not after the whole tail. Two paths
        # can share a single NUL-terminated region -- libHNcommonlib.so has
        # ".../Persistence/onoff.log" immediately followed by
        # ".../Persistence/CarDialy/DrivingDistance.dat" with no terminator
        # between them -- and skipping the tail would leave the second one
        # unpatched. `old not in new` is checked by the caller, so this cannot
        # rescan its own output forever.
        pos = i + len(new)
    return bytes(buf), count


def _uleb(data: bytes, o: int) -> tuple[int, int]:
    """Read a ULEB128. Returns (value, byte_count)."""
    r = s = 0
    n = 0
    while True:
        b = data[o + n]
        n += 1
        r |= (b & 0x7F) << s
        if not b & 0x80:
            return r, n
        s += 7


def _uleb_encode(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def patch_dex(data: bytes, pairs, allow_shorter: bool) -> tuple[bytes, int, str | None]:
    """Substitute inside a dex by walking its string table.

    A dex string is `uleb128 utf16_size` followed by NUL-terminated MUTF-8, so a
    blind byte replace that shortens the content leaves utf16_size too large.
    Dalvik runs dexopt at install time on Android 4.4 and rejects the file, which
    shows up simply as the install failing. So rewrite the length prefix too.

    Both sides of every substitution are ASCII, so the change in UTF-16 code
    units equals the change in bytes; no MUTF-8 arithmetic is needed.

    Returns (data, hits, error).
    """
    buf = bytearray(data)
    count, ids_off = struct.unpack_from("<II", data, 56)
    hits = 0
    for i in range(count):
        str_off = struct.unpack_from("<I", data, ids_off + 4 * i)[0]
        declared, uleb_len = _uleb(data, str_off)
        dstart = str_off + uleb_len
        dend = data.index(b"\x00", dstart)
        old = bytes(data[dstart:dend])
        new = old
        for o, n in pairs:
            if o in new:
                new = new.replace(o, n)
        if new == old:
            continue
        if len(new) > len(old):
            return bytes(buf), hits, (
                f"string {i} would grow ({len(old)} -> {len(new)} bytes), which "
                f"would move every later dex offset")
        if len(new) < len(old) and not allow_shorter:
            return bytes(buf), hits, (
                f"string {i} would shrink and --allow-shorter was not given")
        new_declared = declared - (len(old) - len(new))
        new_uleb = _uleb_encode(new_declared)
        if len(new_uleb) != uleb_len:
            return bytes(buf), hits, (
                f"string {i}: the length prefix would change size "
                f"({uleb_len} -> {len(new_uleb)} bytes), which would move "
                f"later dex offsets")
        buf[str_off:str_off + uleb_len] = new_uleb
        buf[dstart:dend] = new + b"\x00" * (len(old) - len(new))
        hits += 1
    return bytes(buf), hits, None


def verify_dex_strings(data: bytes) -> list[str]:
    """Every string's declared utf16_size must match its content.

    This is the check that would have caught the install failure caused by
    shortening dex strings without fixing their length prefixes.
    """
    problems = []
    count, ids_off = struct.unpack_from("<II", data, 56)
    for i in range(count):
        str_off = struct.unpack_from("<I", data, ids_off + 4 * i)[0]
        declared, uleb_len = _uleb(data, str_off)
        dstart = str_off + uleb_len
        dend = data.index(b"\x00", dstart)
        raw = bytes(data[dstart:dend])
        try:
            actual = len(raw.decode("utf-8"))
        except UnicodeDecodeError:
            problems.append(f"string {i} is not decodable: {raw[:60]!r}")
            continue
        if actual != declared:
            problems.append(
                f"string {i} declares {declared} but holds {actual}: {raw[:60]!r}")
    return problems


def fix_dex_header(data: bytes) -> bytes:
    """Recompute dex signature (SHA-1 of [32:]) then checksum (adler32 of [12:])."""
    buf = bytearray(data)
    buf[12:32] = hashlib.sha1(bytes(buf[32:])).digest()
    buf[8:12] = (zlib.adler32(bytes(buf[12:])) & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(buf)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="directory holding the unpacked APK")
    ap.add_argument("--map", action="append", default=[], metavar="OLD=NEW",
                    help="path substitution; repeatable. "
                         "default: /mnt/extSdCard=/sdcard/n200dt")
    ap.add_argument("--map-file", action="append", default=[], metavar="FILE",
                    help="read OLD=NEW substitutions from FILE, one per line, "
                         "applied in file order; '#' comments and blank lines "
                         "are ignored. Repeatable, and combines with --map.")
    ap.add_argument("--allow-shorter", action="store_true",
                    help="permit NEW shorter than OLD (left-shift + NUL pad)")
    ap.add_argument("--dry-run", action="store_true", help="report, change nothing")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    root = pathlib.Path(args.root)
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 1

    specs = []
    for mf in args.map_file:
        p = pathlib.Path(mf)
        if not p.is_file():
            print(f"error: no such map file: {mf}", file=sys.stderr)
            return 1
        for raw in p.read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if line:
                specs.append(line)
    specs += args.map
    if not specs:
        specs = [f"{o}={n}" for o, n in DEFAULT_MAP]

    pairs = []
    for spec in specs:
        if "=" not in spec:
            print(f"error: --map needs OLD=NEW, got {spec!r}", file=sys.stderr)
            return 1
        old, new = spec.split("=", 1)
        if "/" not in old or "/" not in new or len(old) < 6:
            print(f"error: both sides must look like paths and OLD must be at "
                  f"least 6 bytes, to avoid matching stray bytes: {spec!r}",
                  file=sys.stderr)
            return 1
        if len(new) > len(old):
            print(f"error: replacement is longer than the original, which would "
                  f"shift file offsets and corrupt the ELF/dex:\n"
                  f"       {old!r} ({len(old)} bytes) -> {new!r} ({len(new)} bytes)",
                  file=sys.stderr)
            return 1
        if len(new) < len(old) and not args.allow_shorter:
            print(f"error: {new!r} is shorter than {old!r}. Pick an equal-length "
                  f"path ({len(old)} bytes) or pass --allow-shorter.",
                  file=sys.stderr)
            return 1
        if old in new:
            print(f"error: {old!r} occurs inside its own replacement {new!r}; "
                  f"the substitution would rescan its own output.",
                  file=sys.stderr)
            return 1
        pairs.append((old.encode(), new.encode()))

    targets = sorted(root.glob("lib/*/*.so"))
    dex = sorted(root.glob("classes*.dex"))
    targets += dex
    arsc = root / "resources.arsc"
    if arsc.exists():
        targets.append(arsc)

    if not targets:
        print(f"error: found no .so / .dex under {root}", file=sys.stderr)
        return 1

    total = 0
    touched = 0
    for path in targets:
        data = original = path.read_bytes()
        hits = 0
        is_dex = path.name.startswith("classes") and path.suffix == ".dex"
        if is_dex:
            data, hits, err = patch_dex(data, pairs, args.allow_shorter)
            if err:
                print(f"error: {path.name}: {err}", file=sys.stderr)
                return 1
        else:
            for old, new in pairs:
                if len(new) == len(old):
                    data, n = patch_bytes_equal(data, old, new)
                else:
                    data, n = patch_bytes_shorter(data, old, new)
                hits += n
        if not hits:
            continue
        if is_dex:
            data = fix_dex_header(data)
            problems = verify_dex_strings(data)
            if problems:
                print(f"error: {path.name} is corrupt after patching "
                      f"({len(problems)} bad strings). Android would refuse to "
                      f"install it:", file=sys.stderr)
                for p in problems[:10]:
                    print(f"    {p}", file=sys.stderr)
                return 1
        if path.name == "resources.arsc":
            # Equal-length only; arsc string pools are length-prefixed and we do
            # not rewrite them. Flag it rather than silently risking it.
            if any(len(n) != len(o) for o, n in pairs):
                print(f"error: refusing to shorten strings inside {path.name}",
                      file=sys.stderr)
                return 1
        if len(data) != len(original):
            print(f"error: internal: {path.name} changed size "
                  f"({len(original)} -> {len(data)})", file=sys.stderr)
            return 1
        if not args.dry_run:
            path.write_bytes(data)
        total += hits
        touched += 1
        if not args.quiet:
            tag = "would patch" if args.dry_run else "patched"
            extra = " (+dex checksum/signature)" if path.suffix == ".dex" else ""
            print(f"    {tag} {hits:4d}  {path.relative_to(root)}{extra}")

    if not args.quiet:
        print(f"    ---- {total} occurrences in {touched} files")
        if len(pairs) <= 2:
            # A whole-root move: say where the data has to go.
            for old, new in pairs:
                print(f"    data root: {old.decode()}  ->  {new.decode()}")
            print(f"    put the map data at {pairs[0][1].decode()}/Navkingdom/...")
        else:
            # A curated per-path list (e.g. tools/write_redirects.txt); printing
            # "put the map data at ..." for each entry would be nonsense.
            print(f"    applied {len(pairs)} substitutions")
    if total == 0:
        print("warning: nothing matched; is this the right APK?", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
