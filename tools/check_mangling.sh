#!/usr/bin/env bash
# Confirm stub_players.cpp mangles to exactly the symbols the app imports.
#
# Itanium C++ mangling does not depend on the target architecture, so a host
# compiler answers this question in seconds without an NDK. That makes this the
# fast gate: it catches a wrong parameter type, a missing const, a renamed class
# or a dropped namespace immediately, instead of after a 1 GB NDK download.
#
# The ARM build in stub/build.sh repeats the same check on the real artifact.
#
# Usage: tools/check_mangling.sh [c++-compiler]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CXX="${1:-${CXX:-g++}}"
SRC="$REPO/stub/jni/stub_players.cpp"
EXPECTED="$REPO/stub/expected_symbols.txt"

command -v "$CXX" >/dev/null || { echo "error: no C++ compiler ($CXX)" >&2; exit 2; }

READELF=""
for c in readelf llvm-readelf; do
    command -v "$c" >/dev/null && { READELF="$c"; break; }
done
[ -n "$READELF" ] || { echo "error: no readelf found" >&2; exit 2; }

TMP="$(mktemp -d -t n200mangle.XXXXXX)"
trap 'rm -rf "$TMP"' EXIT

echo "==> host-compiling the stub with $CXX (mangling only; the object is thrown away)"
# Same version script as the real build, so the exported set checked here is
# the exported set the ARM build will produce.
"$CXX" -shared -fPIC -O2 -fno-exceptions -fno-rtti -fvisibility=default \
    -Wl,--version-script="$REPO/stub/jni/stub.map" -Wl,--exclude-libs,ALL \
    -o "$TMP/mangle.so" "$SRC"

"$READELF" -sW --dyn-syms "$TMP/mangle.so" \
  | awk '$7!="UND" && ($5=="GLOBAL"||$5=="WEAK"){print $8}' \
  | sed 's/@.*//' | sort -u > "$TMP/got.txt"

sort -u "$EXPECTED" > "$TMP/want.txt"

MISSING="$(comm -23 "$TMP/want.txt" "$TMP/got.txt" || true)"
EXTRA="$(comm -13 "$TMP/want.txt" "$TMP/got.txt" || true)"

if [ -n "$MISSING" ]; then
    echo "FAIL: these symbols are imported by the app but the stub does not define them:" >&2
    echo "$MISSING" | sed 's/^/    /' >&2
    echo >&2
    echo "Compare against what the stub actually produced:" >&2
    echo "$EXTRA" | sed 's/^/    got: /' >&2
    exit 1
fi

echo "    ok: all $(wc -l < "$TMP/want.txt") required symbols present"

# GCC emits C2 (base-object constructor) alongside C1 for a class with no
# virtual bases. Those aliases are harmless. Anything else is a failure: see the
# comment in stub/jni/stub.map for why a stray export is dangerous here.
UNEXPECTED="$(echo "$EXTRA" | grep -vE 'C2E|^(__bss_start|_edata|_end)$|^$' || true)"
if [ -n "$UNEXPECTED" ]; then
    echo "FAIL: the stub exports symbols nothing asked for:" >&2
    echo "$UNEXPECTED" | sed 's/^/    /' >&2
    echo >&2
    echo "Check that jni/stub.map still localizes everything else." >&2
    exit 1
fi
echo "    ok: no unexpected exports"
exit 0
