#!/usr/bin/env bash
# Build the stub libstagefright_dmb.so and verify its ABI before letting it out.
#
#   ./build.sh                     # silent stub (default)
#   STUB_TRACE=1 ./build.sh        # logcat-traced stub: shows every call the
#                                  # navigation engine makes into DMB/TTS
#   STUB_START_FAILS=1 ./build.sh  # start()/setDataSource() return -1
#
# Needs an Android NDK. Point ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) at it, or
# have ndk-build on PATH. Any NDK that still supports android-19 works; r21e is
# a good last one. r27+ has dropped API 19 and will not work.
set -euo pipefail

cd "$(dirname "$0")"
HERE="$PWD"
OUT="$HERE/out"

# ---------------------------------------------------------------- locate ndk
NDK_BUILD=""
for cand in \
    "${ANDROID_NDK_HOME:-}/ndk-build" \
    "${ANDROID_NDK_ROOT:-}/ndk-build" \
    "${NDK:-}/ndk-build" \
    "$(command -v ndk-build 2>/dev/null || true)"
do
    [ -n "$cand" ] && [ -x "$cand" ] && { NDK_BUILD="$cand"; break; }
done
if [ -z "$NDK_BUILD" ]; then
    echo "error: ndk-build not found." >&2
    echo "       set ANDROID_NDK_HOME=/path/to/android-ndk-r21e (or put ndk-build on PATH)" >&2
    exit 1
fi
NDK_DIR="$(cd "$(dirname "$NDK_BUILD")" && pwd)"
echo "==> NDK: $NDK_DIR"

# ---------------------------------------------------------------- readelf
# Prefer the NDK's own llvm-readelf so the host does not need binutils.
READELF=""
for cand in \
    "$NDK_DIR"/toolchains/llvm/prebuilt/*/bin/llvm-readelf \
    "$(command -v readelf 2>/dev/null || true)" \
    "$(command -v llvm-readelf 2>/dev/null || true)"
do
    [ -n "$cand" ] && [ -x "$cand" ] && { READELF="$cand"; break; }
done
[ -n "$READELF" ] || { echo "error: no readelf/llvm-readelf found" >&2; exit 1; }

# ---------------------------------------------------------------- build
rm -rf "$HERE/libs" "$HERE/obj" "$OUT"
"$NDK_BUILD" -C "$HERE" \
    NDK_PROJECT_PATH="$HERE" \
    APP_BUILD_SCRIPT="$HERE/jni/Android.mk" \
    NDK_APPLICATION_MK="$HERE/jni/Application.mk" \
    STUB_TRACE="${STUB_TRACE:-0}" \
    STUB_START_FAILS="${STUB_START_FAILS:-0}"

SO="$(find "$HERE/libs" -name libstagefright_dmb.so | head -1)"
[ -n "$SO" ] || { echo "error: build produced no libstagefright_dmb.so" >&2; exit 1; }

# ---------------------------------------------------------------- verify
echo
echo "==> verifying ABI of $SO"

"$READELF" -sW --dyn-syms "$SO" \
  | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK"){print $8}' \
  | sed 's/@.*//' | sort -u > "$HERE/.got.txt"

MISSING="$(comm -23 <(sort -u expected_symbols.txt) "$HERE/.got.txt" || true)"
if [ -n "$MISSING" ]; then
    echo "FAIL: the stub does not export every symbol the app imports." >&2
    echo "      missing:" >&2
    echo "$MISSING" | sed 's/^/        /' >&2
    echo >&2
    echo "      The C++ declarations in jni/stub_players.cpp must mangle to" >&2
    echo "      exactly the names in expected_symbols.txt. Check return-type-" >&2
    echo "      independent details: namespace, class name, parameter types," >&2
    echo "      const-ness, and the C1 vs C2 constructor variant." >&2
    rm -f "$HERE/.got.txt"
    exit 1
fi
echo "    ok: all 16 required symbols exported"

# And nothing else. An armeabi-v7a link statically pulls in libunwind/libgcc,
# and re-exporting those would let the app's other libraries bind their unwinder
# to this stub instead of to libgnustl_shared.so. jni/stub.map prevents it; this
# is the check that it actually took effect.
EXTRA="$(comm -13 <(sort -u expected_symbols.txt) "$HERE/.got.txt" \
        | grep -vE 'C2E|^(__bss_start|_edata|_end)$' || true)"
if [ -n "$EXTRA" ]; then
    echo "FAIL: the stub exports symbols nothing asked for:" >&2
    echo "$EXTRA" | sed 's/^/        /' >&2
    echo >&2
    echo "      Mixing these with the app's own C++ runtime is a real hazard --" >&2
    echo "      see the comment in jni/stub.map. Check that the version script" >&2
    echo "      and -Wl,--exclude-libs,ALL are still in LOCAL_LDFLAGS." >&2
    rm -f "$HERE/.got.txt"
    exit 1
fi
echo "    ok: no unexpected exports"

# SONAME must stay libstagefright_dmb.so for the DT_NEEDED entries to resolve.
SONAME="$("$READELF" -d "$SO" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')"
if [ "$SONAME" != "libstagefright_dmb.so" ]; then
    echo "FAIL: SONAME is '$SONAME', must be 'libstagefright_dmb.so'" >&2
    rm -f "$HERE/.got.txt"
    exit 1
fi
echo "    ok: SONAME = $SONAME"

# No private platform libraries may come back in through the back door.
BAD="$("$READELF" -d "$SO" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' \
      | grep -vE '^lib(c|m|dl|log|z|stdc\+\+|android|EGL|GLESv2|GLESv3|jnigraphics)\.so$' || true)"
if [ -n "$BAD" ]; then
    echo "FAIL: stub depends on non-NDK-stable libraries:" >&2
    echo "$BAD" | sed 's/^/        /' >&2
    rm -f "$HERE/.got.txt"
    exit 1
fi
echo "    ok: only NDK-stable dependencies"

rm -f "$HERE/.got.txt"
mkdir -p "$OUT"
cp "$SO" "$OUT/libstagefright_dmb.so"

echo
echo "==> $OUT/libstagefright_dmb.so  ($(stat -c%s "$OUT/libstagefright_dmb.so") bytes)"
echo "    original was 2349248 bytes"
echo
echo "Next:  ../tools/repack.sh ../apk/n200_signed.apk"
