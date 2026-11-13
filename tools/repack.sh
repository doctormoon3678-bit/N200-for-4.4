#!/usr/bin/env bash
# Build a patched n200 APK that can load on a stock Android 4.4 phone.
#
#   1. unpack the original APK
#   2. swap libstagefright_dmb.so for the 16-symbol stub  (removes the whole
#      private-platform ABI surface)
#   3. optionally drop the libraries that become dead weight once the stub is in
#   4. repoint the hard-coded /mnt/extSdCard data root at primary storage
#   5. drop the old signature, rezip, re-sign (JAR / v1, which is what API 19 reads)
#   6. re-run the ABI audit on the result and refuse to ship a failing APK
#
# Usage:
#   tools/repack.sh [options] [original.apk]
#
# Options:
#   --stub PATH        stub .so            (default stub/out/libstagefright_dmb.so)
#   --out PATH         output APK          (default out/n200-note4.apk)
#   --data-root PATH   new data root       (default /sdcard/n200dt)
#                      MUST be exactly 14 bytes -- see tools/patch_paths.py
#   --keep-dmb         do not swap in the stub (for A/B testing only)
#   --no-path-patch    leave /mnt/extSdCard alone (if you symlink it on-device)
#   --writes-to-appdata  keep the map data on the SD card and redirect only the
#                      paths the app WRITES into /data/data/com.npnd.n200/.
#                      Fixes the fprintf-on-NULL crash without root and without
#                      moving gigabytes. See tools/write_redirects.txt.
#   --strip-dead       delete libskik.so, libcorkscrew.so, libgccdemangle.so and
#                      gdbserver, which nothing needs once the stub is in place
#   --store            store entries uncompressed, like the original APK
#   --key PATH         PEM private key     (default keys/n200.key.pem, auto-created)
#   --cert PATH        PEM certificate     (default keys/n200.cert.pem, auto-created)
#   --no-verify        skip the final audit
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

APK=""
STUB="$REPO/stub/out/libstagefright_dmb.so"
OUT="$REPO/out/n200-note4.apk"
DATA_ROOT="/sdcard/n200dt"
KEEP_DMB=0
PATH_PATCH=1
STRIP_DEAD=0
WRITES_TO_APPDATA=0
STORE=0
KEY_PEM="$REPO/keys/n200.key.pem"
CERT_PEM="$REPO/keys/n200.cert.pem"
VERIFY=1

while [ $# -gt 0 ]; do
    case "$1" in
        --stub)        STUB="$2"; shift 2 ;;
        --out)         OUT="$2"; shift 2 ;;
        --data-root)   DATA_ROOT="$2"; shift 2 ;;
        --keep-dmb)    KEEP_DMB=1; shift ;;
        --no-path-patch) PATH_PATCH=0; shift ;;
        --writes-to-appdata) WRITES_TO_APPDATA=1; shift ;;
        --strip-dead)  STRIP_DEAD=1; shift ;;
        --store)       STORE=1; shift ;;
        --key)         KEY_PEM="$2"; shift 2 ;;
        --cert)        CERT_PEM="$2"; shift 2 ;;
        --no-verify)   VERIFY=0; shift ;;
        -h|--help)     sed -n '2,/^set -euo/p' "$0" | sed 's/^# \?//'; exit 0 ;;
        -*)            echo "unknown option: $1" >&2; exit 1 ;;
        *)             APK="$1"; shift ;;
    esac
done

[ -n "$APK" ] || APK="$REPO/apk/n200_signed.apk"
[ -f "$APK" ] || { echo "error: original APK not found: $APK" >&2; exit 1; }

for t in unzip openssl python3; do
    command -v "$t" >/dev/null || { echo "error: missing tool: $t" >&2; exit 1; }
done

if [ "$KEEP_DMB" -eq 0 ] && [ ! -f "$STUB" ]; then
    echo "error: stub not found: $STUB" >&2
    echo "       build it first:  stub/build.sh" >&2
    exit 1
fi

WORK="$(mktemp -d -t n200repack.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

echo "==> unpacking $(basename "$APK")"
unzip -q -o "$APK" -d "$WORK/apk"

LIBDIR="$WORK/apk/lib/armeabi"
[ -d "$LIBDIR" ] || { echo "error: no lib/armeabi in the APK" >&2; exit 1; }

# -------------------------------------------------------------- stub swap
if [ "$KEEP_DMB" -eq 0 ]; then
    OLDSZ=$(stat -c%s "$LIBDIR/libstagefright_dmb.so" 2>/dev/null || echo 0)
    cp "$STUB" "$LIBDIR/libstagefright_dmb.so"
    NEWSZ=$(stat -c%s "$LIBDIR/libstagefright_dmb.so")
    echo "==> libstagefright_dmb.so: $OLDSZ -> $NEWSZ bytes (stub)"
else
    echo "==> keeping the original libstagefright_dmb.so (--keep-dmb)"
fi

# -------------------------------------------------------------- dead weight
# Once the stub is in, nothing references these:
#   libskik.so        <- was needed only by libstagefright_dmb.so
#   libcorkscrew.so   <- was needed only by libstagefright_dmb.so
#   libgccdemangle.so <- was needed only by libcorkscrew.so
#   gdbserver         <- debug build leftover
if [ "$STRIP_DEAD" -eq 1 ]; then
    if [ "$KEEP_DMB" -eq 1 ]; then
        echo "error: --strip-dead cannot be combined with --keep-dmb" >&2
        exit 1
    fi
    FREED=0
    for dead in libskik.so libcorkscrew.so libgccdemangle.so gdbserver; do
        if [ -f "$LIBDIR/$dead" ]; then
            FREED=$((FREED + $(stat -c%s "$LIBDIR/$dead")))
            rm -f "$LIBDIR/$dead"
            echo "    removed lib/armeabi/$dead"
        fi
    done
    echo "==> freed $FREED bytes of now-unreferenced libraries"
fi

# -------------------------------------------------------------- data root
if [ "$WRITES_TO_APPDATA" -eq 1 ]; then
    echo "==> redirecting only the written paths into the app's private directory"
    echo "    (the map database stays on the SD card where it already is)"
    # --skip-dex: classes.dex must not be touched. Its string table is sorted,
    # and /data/... sorts before /mnt/..., so rewriting a path in place breaks
    # the ordering that Dalvik's dexopt verifies at install time. The Java side
    # only checks that the Persistence folder exists, which it still does on the
    # card; the writes that actually crash are all in the native libraries.
    python3 "$REPO/tools/patch_paths.py" "$WORK/apk" \
        --map-file "$REPO/tools/write_redirects.txt" --allow-shorter --skip-dex
elif [ "$PATH_PATCH" -eq 1 ]; then
    echo "==> repointing the data root at $DATA_ROOT"
    REL="${DATA_ROOT#/}"
    python3 "$REPO/tools/patch_paths.py" "$WORK/apk" \
        --map "/mnt/extSdCard=$DATA_ROOT" \
        --map "mnt/extSdCard=$REL"
else
    echo "==> leaving /mnt/extSdCard in place (--no-path-patch)"
    echo "    the app will need that path to exist and be writable on the device"
fi

# -------------------------------------------------------------- repack
echo "==> repacking"
rm -rf "$WORK/apk/META-INF"
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# Rebuild with the original's entry order, per-entry compression method and
# MS-DOS metadata, so the repack differs from a package known to install only in
# the bytes that were deliberately changed.
python3 "$REPO/tools/rezip.py" "$WORK/apk" "$APK" "$WORK/unsigned.apk"

# -------------------------------------------------------------- sign
# v1 (JAR) signing is what Android 4.4 verifies. NOT done with jarsigner: a
# modern JDK writes a CMS SignerInfo carrying signed attributes (contentType,
# messageDigest, signingTime, CMSAlgorithmProtection) with
# sha256WithRSAEncryption as the signature algorithm -- a shape Android 4.4's
# Harmony ASN.1 verifier has had trouble with, and different from the original
# APK, which installs. tools/sign_v1.py reproduces the original's format
# exactly: SHA-256 digests, no signed attributes, rsaEncryption.
#
# The certificate is self-signed with SHA1, like the AOSP testkey certificate
# on the original. Android never validates an APK certificate's own signature;
# this only removes one more difference from a package known to install.
if [ ! -f "$KEY_PEM" ] || [ ! -f "$CERT_PEM" ]; then
    echo "==> creating signing key $KEY_PEM"
    mkdir -p "$(dirname "$KEY_PEM")"
    if ! openssl req -x509 -newkey rsa:2048 -nodes -sha1 -days 10950 \
            -keyout "$KEY_PEM" -out "$CERT_PEM" \
            -subj "/C=KR/O=n200 repack/CN=n200" >/dev/null 2>&1; then
        # Some OpenSSL builds refuse SHA1 for new certificates. The cert's own
        # signature is never checked by Android, so SHA-256 is equally valid.
        openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 10950 \
            -keyout "$KEY_PEM" -out "$CERT_PEM" \
            -subj "/C=KR/O=n200 repack/CN=n200" >/dev/null 2>&1 || {
            echo "error: could not create the signing key" >&2; exit 1; }
    fi
fi

echo "==> signing"
python3 "$REPO/tools/sign_v1.py" --key "$KEY_PEM" --cert "$CERT_PEM" \
    "$WORK/unsigned.apk" "$OUT"

# Independent cross-check with a second verifier, where one is installed.
if command -v jarsigner >/dev/null; then
    if ! vrfout=$(jarsigner -verify "$OUT" 2>&1) || ! grep -q "jar verified" <<<"$vrfout"; then
        echo "$vrfout" >&2
        echo "error: jarsigner does not accept the signature" >&2
        exit 1
    fi
    echo "    jarsigner agrees the signature verifies"
fi

# -------------------------------------------------------------- structural gate
# This is a hard gate, unlike the ABI audit below. It catches the class of bug
# that produces a silent "install failed" -- notably a dex whose string length
# prefixes no longer match its content, which still checksums and signs cleanly.
echo
if ! python3 "$REPO/tools/check_apk.py" "$OUT" --original "$APK"; then
    echo
    echo "error: the repacked APK is structurally broken; refusing to ship it." >&2
    mv -f "$OUT" "$OUT.rejected"
    echo "       moved to $OUT.rejected for inspection." >&2
    exit 1
fi

# -------------------------------------------------------------- audit
if [ "$VERIFY" -eq 1 ]; then
    echo
    if ! python3 "$REPO/tools/verify_abi.py" "$OUT"; then
        echo
        if [ "$KEEP_DMB" -eq 1 ]; then
            # Not a gate here. --keep-dmb means the original vendor library was
            # kept on purpose, so of course its private platform dependencies
            # are still listed. Whether they resolve is a property of the device,
            # and on a Samsung KitKat build they have been observed to.
            echo "This is expected with --keep-dmb: the original"
            echo "libstagefright_dmb.so was kept, so its private platform"
            echo "dependencies are still there. Whether they resolve is up to the"
            echo "device. Drop --keep-dmb to swap in the stub if they do not."
        else
            echo "The repacked APK still carries private platform dependencies." >&2
            echo "It was written to $OUT anyway so you can inspect it." >&2
            exit 1
        fi
    fi
fi

echo
echo "==> $OUT  ($(stat -c%s "$OUT") bytes)"
cat <<EOF

Install:
    # the signature changed from the original (AOSP testkey), so the old copy
    # must go first -- an upgrade in place will be rejected
    adb uninstall com.npnd.n200
    adb install "$OUT"

Map data:
    adb shell mkdir -p $DATA_ROOT
    adb push <your Navkingdom tree> $DATA_ROOT/Navkingdom

Watch it start:
    adb logcat -c
    adb shell am start -n com.npnd.n200/.LocalServiceActivities
    adb logcat -d | grep -iE "AndroidRuntime|linker|dlopen|UnsatisfiedLink|n200"
EOF
