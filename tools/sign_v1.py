#!/usr/bin/env python3
"""Sign an APK with a v1 (JAR) signature in the Android platform's own format.

WHY NOT jarsigner
-----------------
A modern JDK's jarsigner writes a CMS SignerInfo with signed attributes --
contentType, messageDigest, signingTime and CMSAlgorithmProtection
(1.2.840.113549.1.9.52) -- and names the signature algorithm
sha256WithRSAEncryption. Android 4.4 verifies v1 signatures with the old
Apache Harmony ASN.1 code, and that format is exactly the one with a history of
compatibility trouble there: when the signature block fails to decode, the APK
is treated as unsigned and the install is refused.

The original n200_signed.apk, which does install on the target device, carries
a SignerInfo with NO signed attributes and rsaEncryption as the signature
algorithm, over SHA-256 digests. That is what Android's own SignApk/apksigner
produce, and this script reproduces it:

  META-INF/MANIFEST.MF   Manifest-Version / Created-By, then one section per
                         entry with a SHA-256-Digest of the entry's bytes
  META-INF/CERT.SF       SHA-256-Digest-Manifest over the whole manifest, then
                         one section per entry with a SHA-256-Digest of that
                         entry's manifest section
  META-INF/CERT.RSA      detached PKCS#7 SignedData over CERT.SF, made by
                         `openssl cms -sign -noattr`, so: no signed attributes,
                         rsaEncryption

SHA-256 digests are safe for this target: the original APK already uses them and
installs, and Android has accepted them for v1 since API 18.

Manifest lines are wrapped at 72 bytes with single-space continuation lines,
exactly as java.util.jar.Manifest does, because the .SF digests are computed
over the manifest's literal bytes.

Entries keep the input's order, compression method and metadata. The signature
files go first.

Usage:
    sign_v1.py --key key.pem --cert cert.pem <unsigned.apk> <signed.apk>
"""

import argparse
import base64
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

SIG_PREFIXES = ("META-INF/MANIFEST.MF",)
SIG_SUFFIXES = (".SF", ".RSA", ".DSA", ".EC")


def is_sig_file(name: str) -> bool:
    if name in SIG_PREFIXES:
        return True
    return name.startswith("META-INF/") and name.upper().endswith(SIG_SUFFIXES)


def line(text: bytes) -> bytes:
    """One manifest header line, wrapped like java.util.jar.Manifest."""
    out = bytearray(text[:72])
    rest = text[72:]
    while rest:
        out += b"\r\n " + rest[:71]
        rest = rest[71:]
    return bytes(out) + b"\r\n"


def b64sha256(data: bytes) -> bytes:
    return base64.b64encode(hashlib.sha256(data).digest())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", required=True, help="PEM private key")
    ap.add_argument("--cert", required=True, help="PEM certificate")
    ap.add_argument("unsigned")
    ap.add_argument("signed")
    args = ap.parse_args()

    openssl = shutil.which("openssl")
    if not openssl:
        print("error: openssl not found", file=sys.stderr)
        return 1

    with zipfile.ZipFile(args.unsigned) as zin:
        infos = [i for i in zin.infolist()
                 if not i.is_dir() and not is_sig_file(i.filename)]
        blobs = {i.filename: zin.read(i.filename) for i in infos}

    # ---------------------------------------------------------- MANIFEST.MF
    mf = bytearray(line(b"Manifest-Version: 1.0") +
                   line(b"Created-By: 1.0 (Android)") + b"\r\n")
    sections = {}
    for info in infos:
        name = info.filename.encode("utf-8")
        sec = (line(b"Name: " + name) +
               line(b"SHA-256-Digest: " + b64sha256(blobs[info.filename])) +
               b"\r\n")
        sections[info.filename] = sec
        mf += sec
    mf = bytes(mf)

    # ---------------------------------------------------------- CERT.SF
    sf = bytearray(line(b"Signature-Version: 1.0") +
                   line(b"Created-By: 1.0 (Android)") +
                   line(b"SHA-256-Digest-Manifest: " + b64sha256(mf)) + b"\r\n")
    for info in infos:
        name = info.filename.encode("utf-8")
        sf += (line(b"Name: " + name) +
               line(b"SHA-256-Digest: " + b64sha256(sections[info.filename])) +
               b"\r\n")
    sf = bytes(sf)

    # ---------------------------------------------------------- CERT.RSA
    with tempfile.TemporaryDirectory(prefix="n200sign.") as tmp:
        sf_path = pathlib.Path(tmp) / "CERT.SF"
        rsa_path = pathlib.Path(tmp) / "CERT.RSA"
        sf_path.write_bytes(sf)
        r = subprocess.run(
            [openssl, "cms", "-sign", "-binary", "-noattr", "-nosmimecap",
             "-md", "sha256", "-outform", "DER",
             "-signer", args.cert, "-inkey", args.key,
             "-in", str(sf_path), "-out", str(rsa_path)],
            capture_output=True, text=True)
        if r.returncode != 0:
            print("error: openssl cms -sign failed:\n" + r.stderr, file=sys.stderr)
            return 1
        rsa = rsa_path.read_bytes()

        # The signature must verify over the exact .SF bytes we are shipping.
        v = subprocess.run(
            [openssl, "cms", "-verify", "-binary", "-noverify", "-inform", "DER",
             "-in", str(rsa_path), "-content", str(sf_path),
             "-out", "/dev/null"],
            capture_output=True, text=True)
        if v.returncode != 0:
            print("error: the produced signature does not verify:\n" + v.stderr,
                  file=sys.stderr)
            return 1

    # ---------------------------------------------------------- write
    # STORED entries are 4-byte aligned, as zipalign does and as the original
    # APK is (all 16 of its stored entries). Android 4.4 does not require it,
    # but it is the last structural difference from a package that installs.
    # Padding goes in the local header's extra field as zero bytes, which is
    # exactly what the zipalign of that era wrote, so 4.4 reads it fine.
    def write(zout, zi, data):
        if zi.compress_type == zipfile.ZIP_STORED:
            start = zout.fp.tell()
            name_len = len(zi.filename.encode("utf-8"))
            pad = (-(start + 30 + name_len)) % 4
            zi.extra = b"\x00" * pad
        zout.writestr(zi, data)

    with zipfile.ZipFile(args.signed, "w") as zout:
        for name, data in (("META-INF/MANIFEST.MF", mf),
                           ("META-INF/CERT.SF", sf),
                           ("META-INF/CERT.RSA", rsa)):
            zi = zipfile.ZipInfo(name, date_time=(2008, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.create_system = 0
            zi.external_attr = 0
            write(zout, zi, data)
        for info in infos:
            zi = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            zi.compress_type = info.compress_type
            zi.create_system = 0
            zi.external_attr = 0
            write(zout, zi, blobs[info.filename])

    print(f"    signed {len(infos)} entries: SHA-256 digests, "
          f"PKCS#7 without signed attributes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
