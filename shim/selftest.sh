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
SAN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --cc) CC="$2"; shift 2 ;;
        # Buffer handling in here is all hand-rolled against fixed-size stack
        # arrays, and a stray byte on a phone shows up as a force-close with
        # nothing useful in logcat. Run the same scenarios under the
        # sanitizers so it shows up here instead.
        --sanitize) SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
                    shift ;;
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
# shellcheck disable=SC2086
"$CC" -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter $SAN \
      -DN200FS_LIBC='"libc.so.6"' -DRTLD_NOW=2 -DRTLD_DEFAULT=0 \
      $RENAME \
      -c "$HERE/n200fs.c" -o "$WORK/n200fs.o"
# shellcheck disable=SC2086
"$CC" -std=gnu99 -g -O1 -Wall -Wextra -include stdarg.h $SAN \
      -c "$HERE/test_paths.c" -o "$WORK/test_paths.o"
# shellcheck disable=SC2086
"$CC" $SAN "$WORK/n200fs.o" "$WORK/test_paths.o" -ldl -o "$WORK/test_paths"

# The synthetic GPS feed writes to the FIFO path compiled into the engine,
# so the test needs that directory to exist. Skipped where it cannot be made.
# No FIFO is created on purpose: the redirector hands the engine a pipe, so
# the feed must work with nothing on disk.
rm -f /data/data/com.npnd.n200/files/gpsfifo 2>/dev/null || true

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
# 1b. The card is at a mount point no list could have guessed. Only
#     /proc/mounts can find this one -- "/mnt/weird_card_xyz" is deliberately
#     not in CANDIDATES -- so this is what proves the mount-table search works
#     and not merely the fallback list. Needs to be able to mount, so it is
#     skipped where that is not permitted.
MP=/mnt/weird_card_xyz
if mkdir -p "$MP" 2>/dev/null &&
   mkdir -p "$WORK/bindsrc" &&
   mount --bind "$WORK/bindsrc" "$MP" 2>/dev/null; then
    fixture "$MP"
    mkdir -p "$WORK/primary1b"
    run "card found through /proc/mounts at $MP" "$MP" "$WORK/primary1b" || rc=1
    umount "$MP" 2>/dev/null || true
    rmdir "$MP" 2>/dev/null || true
else
    echo "== card found through /proc/mounts"
    echo "  skip  cannot bind-mount here"
    echo
    rmdir "$MP" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 1c. The regression that mattered. KitKat mounts primary storage under
#     several names at once and EXTERNAL_STORAGE names only one, so a search
#     for "not primary storage" used to pick internal storage under a second
#     name, find the n200dt tree an older build left there, and use that as
#     the card -- often a half-deleted copy, which then crashes the app on the
#     first missing file. The card must win.
CARDMP=/mnt/weird_card_xyz
ALIAS=/storage/emulated/0
# The leftover is mounted FIRST, because that is the order a phone has it in:
# internal storage comes up at boot, the card later. /proc/mounts is in mount
# order, so a search that takes the first match it finds takes the leftover --
# which is what used to happen, and why this test would pass by accident if
# the two were mounted the other way round.
if mkdir -p "$CARDMP" "$ALIAS" 2>/dev/null &&
   mkdir -p "$WORK/card2" "$WORK/leftover/n200dt/Navkingdom/Navi" &&
   mount --bind "$WORK/leftover" "$ALIAS" 2>/dev/null &&
   mount --bind "$WORK/card2" "$CARDMP" 2>/dev/null; then
    fixture "$CARDMP"
    # The leftover is deliberately incomplete, exactly as it would be on a
    # phone that ran out of room: a Navkingdom with a Navi/ but no map.
    mkdir -p "$WORK/primary1c"
    run "card beats a leftover tree on a primary-storage alias" \
        "$CARDMP" "$WORK/primary1c" || rc=1
    umount "$ALIAS" 2>/dev/null || true
    umount "$CARDMP" 2>/dev/null || true
    rmdir "$CARDMP" 2>/dev/null || true
else
    echo "== card beats a leftover tree on a primary-storage alias"
    echo "  skip  cannot bind-mount here"
    echo
    umount "$ALIAS" 2>/dev/null || true
    umount "$CARDMP" 2>/dev/null || true
    rmdir "$CARDMP" 2>/dev/null || true
fi

# ---------------------------------------------------------------------------
# 1d. A directory called Navkingdom that holds neither Navi/ nor Common/ is
#     not a map tree. Latching onto one means every read misses, fopen returns
#     NULL and the app dereferences it -- the original crash.
if mkdir -p "$CARDMP" 2>/dev/null &&
   mkdir -p "$WORK/decoy/Navkingdom" "$WORK/primary1d/n200dt" &&
   mount --bind "$WORK/decoy" "$CARDMP" 2>/dev/null; then
    fixture "$WORK/primary1d/n200dt"
    run "an empty Navkingdom is not mistaken for the map tree" \
        "$WORK/primary1d/n200dt" "$WORK/primary1d" || rc=1
    umount "$CARDMP" 2>/dev/null || true
    rmdir "$CARDMP" 2>/dev/null || true
else
    echo "== an empty Navkingdom is not mistaken for the map tree"
    echo "  skip  cannot bind-mount here"
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
