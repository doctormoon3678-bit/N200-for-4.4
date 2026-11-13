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
    ap.add_argument("--replaced", action="append", metavar="NAME",
                    help="entry this repack replaced or removed on purpose, so "
                         "a size change there is expected rather than the sign "
                         "of an in-place patch gone wrong; repeatable")
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
            # zipalign state. Not required by Android 4.4, so a note rather
            # than a failure, but the original APK is aligned and a repack
            # should not differ from it gratuitously.
            with open(apk, "rb") as fp:
                misaligned = []
                stored = [i for i in z.infolist() if i.compress_type == 0]
                for i in stored:
                    fp.seek(i.header_offset)
                    h = fp.read(30)
                    n, e = struct.unpack_from("<HH", h, 26)
                    if (i.header_offset + 30 + n + e) % 4:
                        misaligned.append(i.filename)
            if misaligned:
                print(f"  note  {len(misaligned)} of {len(stored)} stored entries "
                      f"are not 4-byte aligned (not required on 4.4)")
            else:
                ok(f"all {len(stored)} stored entries 4-byte aligned, like the original")
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

        # ---------------------------------------------- v1, self-contained
        # Re-derive what Android's JarVerifier checks, without depending on a
        # JDK: every entry's digest in MANIFEST.MF, the .SF digest over the
        # whole manifest, and a PKCS#7 block that verifies over the .SF bytes.
        # Also the block's *shape*, because that is what differed between the
        # original APK (installs) and the jarsigner output (did not): signed
        # attributes and CMSAlgorithmProtection.
        import base64
        meta = root / "META-INF"
        mf_path = meta / "MANIFEST.MF"
        sfs = sorted(meta.glob("*.SF")) if meta.is_dir() else []
        blocks = sorted(p for p in meta.glob("*")
                        if p.suffix.upper() in (".RSA", ".DSA", ".EC")) if meta.is_dir() else []
        if not mf_path.is_file() or not sfs or not blocks:
            bad("v1 signature incomplete (need MANIFEST.MF, a .SF and a signature block)")
        else:
            mf_raw = mf_path.read_bytes()
            # unfold continuation lines to read name -> digest
            text = mf_raw.decode("utf-8").replace("\r\n ", "")
            digests = {}
            name = None
            for ln in text.split("\r\n"):
                if ln.startswith("Name: "):
                    name = ln[6:]
                elif ln.startswith("SHA-256-Digest: ") and name:
                    digests[name] = ln[len("SHA-256-Digest: "):]
                elif not ln:
                    name = None
            mismatch = []
            missing = []
            for f in sorted(root.rglob("*")):
                if not f.is_file():
                    continue
                rel = str(f.relative_to(root)).replace("\\", "/")
                if rel.startswith("META-INF/"):
                    continue
                want = digests.get(rel)
                if want is None:
                    missing.append(rel)
                    continue
                got = base64.b64encode(hashlib.sha256(f.read_bytes()).digest()).decode()
                if got != want:
                    mismatch.append(rel)
            if missing:
                bad(f"{len(missing)} entries absent from MANIFEST.MF, e.g. {missing[:3]}")
            if mismatch:
                bad(f"{len(mismatch)} entries whose content does not match "
                    f"MANIFEST.MF, e.g. {mismatch[:3]}")
            if not missing and not mismatch:
                ok(f"MANIFEST.MF digests match all {len(digests)} entries")

            sf_raw = sfs[0].read_bytes()
            m = re.search(rb"SHA-256-Digest-Manifest: ([A-Za-z0-9+/=]+)", sf_raw)
            want_mf = m.group(1).decode() if m else None
            got_mf = base64.b64encode(hashlib.sha256(mf_raw).digest()).decode()
            if want_mf == got_mf:
                ok(f"{sfs[0].name} digest of MANIFEST.MF matches")
            else:
                bad(f"{sfs[0].name} does not match MANIFEST.MF")

            openssl = shutil.which("openssl")
            if openssl:
                v = subprocess.run(
                    [openssl, "cms", "-verify", "-binary", "-noverify",
                     "-inform", "DER", "-in", str(blocks[0]),
                     "-content", str(sfs[0]), "-out", "/dev/null"],
                    capture_output=True, text=True)
                if v.returncode == 0:
                    ok(f"{blocks[0].name} verifies over {sfs[0].name}")
                else:
                    bad(f"{blocks[0].name} does not verify over {sfs[0].name}")
                parsed = subprocess.run(
                    [openssl, "asn1parse", "-inform", "DER", "-in", str(blocks[0])],
                    capture_output=True, text=True).stdout
                attrs = [a for a in ("contentType", "messageDigest", "signingTime",
                                     "1.2.840.113549.1.9.52")
                         if re.search(r":%s\s*$" % re.escape(a), parsed, re.MULTILINE)]
                if attrs:
                    bad(f"{blocks[0].name} carries signed attributes {attrs}; "
                        f"the original APK's block has none. Sign with "
                        f"tools/sign_v1.py, not jarsigner")
                else:
                    ok(f"{blocks[0].name} has no signed attributes, matching "
                       f"the original's format")
            else:
                print("  skip  PKCS#7 checks (no openssl)")

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
                # Entries this repack replaced outright, rather than patched in
                # place. A swapped-in stub is a different file and is supposed
                # to be a different size; the point of this check is to catch a
                # byte patch that moved offsets, so those are reported
                # separately instead of failing.
                replaced = set(args.replaced or [])
                grew, swapped = [], []
                for name in names:
                    if name.startswith("META-INF/"):
                        continue
                    if name in osizes and osizes[name] != 0:
                        with zipfile.ZipFile(apk) as z2:
                            n = z2.getinfo(name).file_size
                        if n != osizes[name]:
                            (swapped if name in replaced else grew).append(
                                f"{name}: {osizes[name]} -> {n}")
                for s in swapped:
                    ok(f"replaced on purpose, not patched: {s}")
                gone = sorted(n for n in osizes
                              if n not in names and not n.startswith("META-INF/"))
                for g in gone:
                    if g in replaced:
                        ok(f"removed on purpose: {g}")
                    else:
                        bad(f"entry disappeared from the APK: {g}")
                if grew:
                    for g in grew:
                        bad(f"size changed, so an in-place patch moved offsets: {g}")
                else:
                    ok(f"no in-place patch changed any entry's size "
                       f"({len(names) - len(swapped)} entries)")
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
