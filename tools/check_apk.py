#!/usr/bin/env python3
"""Validate a repacked APK before it goes anywhere near a device.

This exists because a repack shipped with a corrupt classes.dex: two strings had
been shortened in place without fixing their ULEB128 utf16_size prefixes. The
dex adler32 checksum and SHA-1 signature were both recomputed and both verified,
so every check in the pipeline passed -- and Android still refused to install
the APK, because Dalvik runs dexopt at install time on 4.4 and validates the
string table. "The install fails" was the only symptom.

So the checks here are the ones that would have caught that, plus the cheap
structural checks worth having around it:

  * every dex string's declared utf16_size matches its content
  * the dex string table is still sorted (dexopt rejects it otherwise)
  * the dex adler32 checksum and SHA-1 signature match
  * every native library is still a parseable 32-bit ARM ELF
  * the zip itself is intact and holds the files an APK needs
  * the JAR signature verifies, with a digest Android 4.4 accepts

Usage:
    check_apk.py <apk> [--original <apk>]

With --original, file sizes are compared against it: an in-place byte patch must
not change the size of anything.

Exit status is 0 only when every check passes.
"""

import argparse
import hashlib
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from patch_paths import verify_dex_order, verify_dex_strings  # noqa: E402

REQUIRED = ["AndroidManifest.xml", "classes.dex", "resources.arsc"]
# Digests Android's v1 verifier handles at API 19. SHA-512 is API 21+, and
# SHA-384 is not documented as supported at all -- a modern JDK will pick it by
# default, and the install then fails with no useful message.
OK_DIGESTS = {"SHA1", "SHA-256"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("apk")
    ap.add_argument("--original", help="compare entry sizes against this APK")
    args = ap.parse_args()

    apk = pathlib.Path(args.apk)
    if not apk.is_file():
        print(f"error: {apk} not found", file=sys.stderr)
        return 2

    failures = []

    def ok(msg):
        print(f"  ok    {msg}")

    def bad(msg):
        print(f"  FAIL  {msg}")
        failures.append(msg)

    print(f"==> checking {apk} ({apk.stat().st_size} bytes)")

    # ---------------------------------------------------------------- zip
    try:
        with zipfile.ZipFile(apk) as z:
            names = z.namelist()
            broken = z.testzip()
            if broken:
                bad(f"zip entry is corrupt: {broken}")
            else:
                ok(f"zip intact, {len(names)} entries")
            for req in REQUIRED:
                if req not in names:
                    bad(f"missing {req}")
            if all(r in names for r in REQUIRED):
                ok("AndroidManifest.xml, classes.dex, resources.arsc present")
            if not any(n.startswith("lib/") and n.endswith(".so") for n in names):
                bad("no native libraries")
            tmp = tempfile.mkdtemp(prefix="n200check.")
            z.extractall(tmp)
    except zipfile.BadZipFile as exc:
        bad(f"not a readable zip: {exc}")
        return 1

    root = pathlib.Path(tmp)
    try:
        # ------------------------------------------------------------ dex
        for dex in sorted(root.glob("classes*.dex")):
            d = dex.read_bytes()
            if d[:4] != b"dex\n":
                bad(f"{dex.name} has no dex magic")
                continue
            declared_size = struct.unpack_from("<I", d, 32)[0]
            if declared_size != len(d):
                bad(f"{dex.name} header size {declared_size} != actual {len(d)}")
            sig = hashlib.sha1(d[32:]).digest()
            if sig != d[12:32]:
                bad(f"{dex.name} SHA-1 signature does not match its content")
            chk = zlib.adler32(d[12:]) & 0xFFFFFFFF
            if chk != struct.unpack_from("<I", d, 8)[0]:
                bad(f"{dex.name} adler32 checksum does not match its content")
            problems = verify_dex_strings(d)
            order = verify_dex_order(d)
            if problems:
                bad(f"{dex.name}: {len(problems)} strings whose declared "
                    f"utf16_size disagrees with their content -- dexopt will "
                    f"reject this at install time")
                for p in problems[:8]:
                    print(f"          {p}")
            if order:
                bad(f"{dex.name}: string_ids are no longer sorted "
                    f"({len(order)} violation(s)) -- dexopt will reject this at "
                    f"install time")
                for p in order[:8]:
                    print(f"          {p}")
            if not problems and not order:
                ok(f"{dex.name} string table sorted and consistent, "
                   f"checksum and signature match")

        # ------------------------------------------------------------ elf
        readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
        libs = sorted(root.glob("lib/*/*.so"))
        if not readelf:
            print("  skip  ELF checks (no readelf)")
        else:
            elf_bad = []
            for so in libs:
                out = subprocess.run([readelf, "-h", str(so)],
                                     capture_output=True, text=True)
                if out.returncode != 0 or "ELF32" not in out.stdout:
                    elf_bad.append(f"{so.name}: not a valid ELF32")
                    continue
                m = re.search(r"Machine:\s+(.+)", out.stdout)
                if not m or "ARM" not in m.group(1).upper():
                    elf_bad.append(f"{so.name}: machine is "
                                   f"{m.group(1).strip() if m else '?'}, not ARM")
                if subprocess.run([readelf, "-d", str(so)],
                                  capture_output=True).returncode != 0:
                    elf_bad.append(f"{so.name}: dynamic section unreadable")
            if elf_bad:
                for e in elf_bad:
                    bad(e)
            else:
                ok(f"all {len(libs)} native libraries are valid 32-bit ARM ELF")

        # ------------------------------------------------- signature
        jarsigner = shutil.which("jarsigner")
        if not jarsigner:
            print("  skip  signature check (no jarsigner)")
        else:
            out = subprocess.run([jarsigner, "-verify", str(apk)],
                                 capture_output=True, text=True)
            if "jar verified" in out.stdout:
                ok("JAR signature verifies")
            else:
                bad("JAR signature does not verify")
            mf = root / "META-INF" / "MANIFEST.MF"
            if mf.is_file():
                used = set(re.findall(r"^(SHA-?\w+)-Digest:", mf.read_text(),
                                      re.MULTILINE))
                unusable = used - OK_DIGESTS
                if unusable:
                    bad(f"digest {sorted(unusable)} may be rejected at API 19; "
                        f"sign with -digestalg SHA-256")
                elif used:
                    ok(f"digest {sorted(used)} is fine for API 19")
            else:
                bad("no META-INF/MANIFEST.MF -- the APK is unsigned")

        # ------------------------------------------------- sizes
        if args.original:
            orig = pathlib.Path(args.original)
            if not orig.is_file():
                bad(f"--original {orig} not found")
            else:
                with zipfile.ZipFile(orig) as z:
                    osizes = {i.filename: i.file_size for i in z.infolist()}
                grew = []
                for name in names:
                    if name.startswith("META-INF/"):
                        continue
                    if name in osizes and osizes[name] != 0:
                        with zipfile.ZipFile(apk) as z2:
                            n = z2.getinfo(name).file_size
                        if n != osizes[name]:
                            grew.append(f"{name}: {osizes[name]} -> {n}")
                if grew:
                    for g in grew:
                        bad(f"size changed, so an in-place patch moved offsets: {g}")
                else:
                    ok("no entry changed size relative to the original")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if failures:
        print(f"RESULT: FAIL -- {len(failures)} problem(s). Do not install this.")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
