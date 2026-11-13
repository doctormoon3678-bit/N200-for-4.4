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
#   --keystore PATH    signing keystore    (default keys/n200.jks, auto-created)
#   --alias NAME       key alias           (default n200)
#   --storepass PASS   store password      (default n200key)
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
KEYSTORE="$REPO/keys/n200.jks"
ALIAS="n200"
STOREPASS="n200key"
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
        --keystore)    KEYSTORE="$2"; shift 2 ;;
        --alias)       ALIAS="$2"; shift 2 ;;
        --storepass)   STOREPASS="$2"; shift 2 ;;
        --no-verify)   VERIFY=0; shift ;;
        -h|--help)     sed -n '2,/^set -euo/p' "$0" | sed 's/^# \?//'; exit 0 ;;
        -*)            echo "unknown option: $1" >&2; exit 1 ;;
        *)             APK="$1"; shift ;;
    esac
done

[ -n "$APK" ] || APK="$REPO/apk/n200_signed.apk"
[ -f "$APK" ] || { echo "error: original APK not found: $APK" >&2; exit 1; }

for t in unzip zip jarsigner keytool python3; do
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
    python3 "$REPO/tools/patch_paths.py" "$WORK/apk" \
        --map-file "$REPO/tools/write_redirects.txt" --allow-shorter
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
ZIPFLAGS=(-q -X)
if [ "$STORE" -eq 1 ]; then ZIPFLAGS+=(-0); else ZIPFLAGS+=(-9); fi
# Feed entry names in explicitly: "zip -r archive ." prefixes every name with
# "./" in some zip builds, and Android looks entries up by exact name.
( cd "$WORK/apk" && find . -type f | sed 's|^\./||' | LC_ALL=C sort \
    | zip "${ZIPFLAGS[@]}" "$WORK/unsigned.apk" -@ )

# -------------------------------------------------------------- sign
# JAR signing (APK Signature Scheme v1) is what Android 4.4 verifies.
#
# The digest and signature algorithms must be pinned, NOT left to the JDK.
# Android's v1 verifier accepts a SHA-256 digest and SHA256withRSA from API 18
# onwards, and this APK's minSdkVersion is 19, so SHA-256 is the right choice.
# Left alone, JDK 21 picks SHA-384, which API 19 is not documented to handle and
# which can make the install fail with INSTALL_PARSE_FAILED_NO_CERTIFICATES.
# SHA1 is not an option either: recent JDKs refuse to emit it at all.
if [ ! -f "$KEYSTORE" ]; then
    echo "==> creating signing key $KEYSTORE"
    mkdir -p "$(dirname "$KEYSTORE")"
    if ! ktout=$(keytool -genkeypair -alias "$ALIAS" \
            -keyalg RSA -keysize 2048 -sigalg SHA256withRSA -validity 10950 \
            -keystore "$KEYSTORE" -storepass "$STOREPASS" -keypass "$STOREPASS" \
            -dname "CN=n200 repack, OU=none, O=none, C=KR" 2>&1); then
        echo "$ktout" >&2
        echo "error: could not create the signing key" >&2
        exit 1
    fi
fi

echo "==> signing"
cp "$WORK/unsigned.apk" "$OUT"
if ! sigout=$(jarsigner -keystore "$KEYSTORE" -storepass "$STOREPASS" \
        -keypass "$STOREPASS" -digestalg SHA-256 -sigalg SHA256withRSA \
        "$OUT" "$ALIAS" 2>&1); then
    echo "$sigout" >&2
    echo "error: signing failed" >&2
    exit 1
fi
if ! vrfout=$(jarsigner -verify "$OUT" 2>&1) || ! grep -q "jar verified" <<<"$vrfout"; then
    echo "$vrfout" >&2
    echo "error: the signature does not verify" >&2
    exit 1
fi
echo "    signature verifies"

# Fail loudly rather than hand over an APK API 19 may refuse to parse.
if unzip -p "$OUT" META-INF/MANIFEST.MF | grep -qE "SHA-(384|512)-Digest"; then
    echo "error: signed with a digest Android 4.4 may reject. Your jarsigner" >&2
    echo "       ignored -digestalg SHA-256; sign with an older JDK or apksigner" >&2
    echo "       (apksigner sign --v1-signing-enabled true --v2-signing-enabled false)" >&2
    exit 1
fi
echo "    digest: $(unzip -p "$OUT" META-INF/MANIFEST.MF | grep -m1 -oE 'SHA-?[0-9]*-Digest')"

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
