#!/usr/bin/env bash
# Build the stub libstagefright_dmb.so and verify its ABI before letting it out.
#
#   ./build.sh                     # silent stub (default)
#   STUB_TRACE=1 ./build.sh        # logcat-traced stub: shows every call the
#                                  # navigation engine makes into DMB/TTS
#   STUB_START_FAILS=1 ./build.sh  # start()/setDataSource() return -1
#
# Builds with an Android NDK when one is available: point ANDROID_NDK_HOME (or
# ANDROID_NDK_ROOT) at it, or have ndk-build on PATH. Any NDK that still
# supports android-19 works; r21e is a good last one, r27+ has dropped API 19.
#
# With no NDK it falls back to plain clang, the same way shim/build.sh builds
# the redirector. That works because this stub is deliberately bare -- no STL,
# no exceptions, no RTTI, empty bodies, and not one platform header -- so there
# is nothing an NDK sysroot would have to provide. The gates below are the same
# either way, and they are what decides whether the result is usable.
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

CXX="${CXX:-clang++}"
if [ -z "$NDK_BUILD" ] && ! command -v "$CXX" >/dev/null; then
    echo "error: neither ndk-build nor $CXX found." >&2
    echo "       set ANDROID_NDK_HOME=/path/to/android-ndk-r21e, put ndk-build" >&2
    echo "       on PATH, or install a clang that can target ARM." >&2
    exit 1
fi

# ---------------------------------------------------------------- readelf
# Prefer the NDK's own llvm-readelf so the host does not need binutils.
READELF=""
for cand in \
    ${NDK_BUILD:+"$(dirname "$NDK_BUILD")"/toolchains/llvm/prebuilt/*/bin/llvm-readelf} \
    "$(command -v readelf 2>/dev/null || true)" \
    "$(command -v llvm-readelf 2>/dev/null || true)"
do
    [ -n "$cand" ] && [ -x "$cand" ] && { READELF="$cand"; break; }
done
[ -n "$READELF" ] || { echo "error: no readelf/llvm-readelf found" >&2; exit 1; }

# ---------------------------------------------------------------- build
rm -rf "$HERE/libs" "$HERE/obj" "$OUT"

if [ -n "$NDK_BUILD" ]; then
    NDK_DIR="$(cd "$(dirname "$NDK_BUILD")" && pwd)"
    echo "==> NDK: $NDK_DIR"
    "$NDK_BUILD" -C "$HERE" \
        NDK_PROJECT_PATH="$HERE" \
        APP_BUILD_SCRIPT="$HERE/jni/Android.mk" \
        NDK_APPLICATION_MK="$HERE/jni/Application.mk" \
        STUB_TRACE="${STUB_TRACE:-0}" \
        STUB_START_FAILS="${STUB_START_FAILS:-0}"
    SO="$(find "$HERE/libs" -name libstagefright_dmb.so | head -1)"
else
    echo "==> no NDK; building with $CXX"
    TARGET=armv7-none-linux-androideabi
    mkdir -p "$HERE/libs/armeabi-v7a"
    SO="$HERE/libs/armeabi-v7a/libstagefright_dmb.so"
    TMPD="$(mktemp -d -t n200stub.XXXXXX)"
    trap 'rm -rf "$TMPD"' EXIT

    # liblog only, and only for the tracing build. The silent stub calls
    # nothing at all, so it needs no stub libraries to link against.
    printf 'void __android_log_print(void){}\n' > "$TMPD/log.c"
    "$CXX" --target="$TARGET" -fPIC -nostdlib -shared -fuse-ld=lld -w \
           -x c "$TMPD/log.c" -Wl,-soname,liblog.so -o "$TMPD/liblog.so"
    # The trace build reaches n200fs_note through dlsym, so it needs libdl.
    printf 'void dlsym(void){}\n' > "$TMPD/dl.c"
    "$CXX" --target="$TARGET" -fPIC -nostdlib -shared -fuse-ld=lld -w \
           -x c "$TMPD/dl.c" -Wl,-soname,libdl.so -o "$TMPD/libdl.so"

    "$CXX" --target="$TARGET" -fPIC -Os \
           -ffreestanding -fno-builtin -fno-exceptions -fno-rtti \
           -fno-stack-protector -Wall -Wextra \
           -DSTUB_TRACE="${STUB_TRACE:-0}" \
           -DSTUB_START_FAILS="${STUB_START_FAILS:-0}" \
           -c "$HERE/jni/stub_players.cpp" -o "$TMPD/stub.o"

    "$CXX" --target="$TARGET" -shared -nostdlib -fuse-ld=lld \
           -Wl,-soname,libstagefright_dmb.so \
           -Wl,--version-script="$HERE/jni/stub.map" \
           -Wl,--no-undefined \
           "$TMPD/stub.o" "$TMPD/liblog.so" "$TMPD/libdl.so" -o "$SO"
fi

[ -n "$SO" ] || { echo "error: build produced no libstagefright_dmb.so" >&2; exit 1; }

# ------------------------------------------------- the textlayoutshim stub
# Built with clang either way: it is two functions returning constants, and
# ndk-build is not worth wiring up for that. Only meaningful alongside the
# libstagefright_dmb stub -- see the header comment in its source.
TLS_SO=""
if command -v "${CC:-clang}" >/dev/null; then
    TLSD="$(mktemp -d -t n200tls.XXXXXX)"
    TLS_SO="$HERE/libs/libtextlayoutshim.so"
    mkdir -p "$HERE/libs"
    "${CC:-clang}" --target=armv7-none-linux-androideabi -fPIC -Os \
        -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra \
        -c "$HERE/jni/stub_textlayoutshim.c" -o "$TLSD/tls.o"
    printf '{ global: forc; noop_vtable_fn; local: *; };\n' > "$TLSD/tls.map"
    "${CC:-clang}" --target=armv7-none-linux-androideabi -shared -nostdlib \
        -fuse-ld=lld -Wl,-soname,libtextlayoutshim.so \
        -Wl,--version-script="$TLSD/tls.map" -Wl,--no-undefined \
        "$TLSD/tls.o" -o "$TLS_SO"
    rm -rf "$TLSD"
fi

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
# Every undefined symbol must have a DT_NEEDED that can supply it. A missing
# one is not a build error -- the library links fine and then fails to dlopen
# on the device, which is indistinguishable from the ABI problem this stub
# exists to remove.
UND=$("$READELF" -sW --dyn-syms "$SO" | awk '$7=="UND" && $8!=""{sub(/@.*/,"",$8); print $8}' | sort -u)
DEPS=" $("$READELF" -d "$SO" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' ') "
for u in $UND; do
    case "$u" in
        __android_log_print) want=liblog.so ;;
        dlsym|dlopen|dlerror) want=libdl.so ;;
        *) echo "FAIL: unexpected undefined symbol $u" >&2; exit 1 ;;
    esac
    case "$DEPS" in
        *" $want "*) ;;
        *) echo "FAIL: $u needs $want but DT_NEEDED is[$DEPS]" >&2; exit 1 ;;
    esac
done
echo "    ok: every undefined symbol is covered by a DT_NEEDED"

cp "$SO" "$OUT/libstagefright_dmb.so"
if [ -n "$TLS_SO" ] && [ -f "$TLS_SO" ]; then
    cp "$TLS_SO" "$OUT/libtextlayoutshim.so"
    tls_exp=$("$READELF" -sW --dyn-syms "$TLS_SO" \
        | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK"){sub(/@.*/,"",$8); print $8}' \
        | sort -u | tr '\n' ' ')
    tls_dep=$("$READELF" -d "$TLS_SO" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' ')
    if [ "$(echo $tls_exp)" != "forc noop_vtable_fn" ]; then
        echo "FAIL: libtextlayoutshim stub exports [$tls_exp], expected forc + noop_vtable_fn" >&2
        exit 1
    fi
    if [ -n "$tls_dep" ]; then
        echo "FAIL: libtextlayoutshim stub should need nothing, needs [$tls_dep]" >&2
        exit 1
    fi
    echo "    ok: libtextlayoutshim stub exports forc + noop_vtable_fn, needs nothing"
    echo "==> $OUT/libtextlayoutshim.so  ($(stat -c%s "$TLS_SO") bytes)"
    echo "    original was 21613 bytes"
fi

echo
echo "==> $OUT/libstagefright_dmb.so  ($(stat -c%s "$OUT/libstagefright_dmb.so") bytes)"
echo "    original was 2349248 bytes"
echo
echo "Next:  ../tools/repack.sh ../apk/n200_signed.apk"
