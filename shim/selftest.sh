#!/usr/bin/env bash
# Run n200fs.c against the host's libc and check that it redirects what it
# should, where it should.
#
# The shim is written so this is possible: it includes no libc headers and
# dereferences no kernel structure, so the same source that becomes the ARM
# library compiles natively. Its entry points are renamed at compile time
# (-Dfopen=n200_fopen ...) so the real libc stays reachable alongside them.
# Nothing is stubbed out -- including the load-time constructor, which is why
# each scenario builds its fixture first and then starts a fresh process.
#
# Usage: shim/selftest.sh [--cc clang]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
while [ $# -gt 0 ]; do
    case "$1" in
        --cc) CC="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

WORK="$(mktemp -d -t n200fsTest.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

RENAME=""
for s in fopen open stat access opendir mkdir rmdir unlink remove rename \
         utimes statfs system; do
    RENAME="$RENAME -D$s=n200_$s"
done

echo "==> building the host self-test"
# shellcheck disable=SC2086
"$CC" -std=gnu99 -g -O0 -Wall -Wextra -Wno-unused-parameter \
      -DN200FS_LIBC='"libc.so.6"' -DRTLD_NOW=2 -DRTLD_DEFAULT=0 \
      $RENAME \
      -c "$HERE/n200fs.c" -o "$WORK/n200fs.o"
"$CC" -std=gnu99 -g -O0 -Wall -Wextra -include stdarg.h \
      -c "$HERE/test_paths.c" -o "$WORK/test_paths.o"
"$CC" "$WORK/n200fs.o" "$WORK/test_paths.o" -ldl -o "$WORK/test_paths"

run() {                      # run <label> <card-dir> <primary-dir>
    local label="$1" card="$2" primary="$3"
    N200T_CARD="$card" N200T_WRITE="$primary/n200dt" \
    EXTERNAL_STORAGE="$primary" "$WORK/test_paths" "$label"
}

fixture() {                  # fixture <root>  -- lay down a fake map tree
    mkdir -p "$1/Navkingdom/Common/DB_MAP" "$1/Navkingdom/Navi/CONF"
    printf 'MAPDATA'  > "$1/Navkingdom/Common/DB_MAP/NF60.sti"
    printf 'cfg'      > "$1/Navkingdom/Navi/CONF/naviConfig_N200.txt"
}

rc=0

# ---------------------------------------------------------------------------
# 1. The real thing: the map tree on a card at a mount point from the built-in
#    candidate list, writes on primary storage. Needs to create /storage/...,
#    so it is skipped where that is not permitted.
if mkdir -p /storage/extSdCard 2>/dev/null; then
    rm -rf /storage/extSdCard/Navkingdom
    fixture /storage/extSdCard
    mkdir -p "$WORK/primary1"
    run "card auto-detected at /storage/extSdCard" \
        /storage/extSdCard "$WORK/primary1" || rc=1
    rm -rf /storage/extSdCard/Navkingdom
    rmdir /storage/extSdCard /storage 2>/dev/null || true
else
    echo "== card auto-detected at /storage/extSdCard"
    echo "  skip  cannot create /storage/extSdCard here"
    echo
fi

# ---------------------------------------------------------------------------
# 2. n200fs.conf names a card this build has never heard of.
mkdir -p "$WORK/primary2/n200dt" "$WORK/oddcard"
fixture "$WORK/oddcard"
printf 'data=%s\n' "$WORK/oddcard" > "$WORK/primary2/n200dt/n200fs.conf"
run "card named by n200fs.conf" "$WORK/oddcard" "$WORK/primary2" || rc=1

# ---------------------------------------------------------------------------
# 3. No card at all: everything on primary storage, which is what a build
#    without this library did. Reads and writes share a root here, so the
#    "card copy is untouched" check is the overlay reading back its own write.
mkdir -p "$WORK/primary3"
fixture "$WORK/primary3/n200dt"
run "no card -- primary storage only" \
    "$WORK/primary3/n200dt" "$WORK/primary3" || rc=1

echo
if [ "$rc" -eq 0 ]; then
    echo "self-test PASS"
else
    echo "self-test FAIL" >&2
fi
exit "$rc"
