#!/usr/bin/env bash
# Build libn200fs.so, the runtime data-root redirector.
#
# No NDK. The shim includes no libc headers and dereferences no kernel
# structure, so all a build needs is a clang that can emit ARM and a linker.
# The handful of symbols it imports are satisfied by stub libraries generated
# here, purely so the linker records the right DT_NEEDED entries -- at runtime
# the real libc.so, libdl.so and liblog.so are what resolve them.
#
# Usage:
#   shim/build.sh [--cc clang] [--out shim/out/libn200fs.so]
#
# GPS_DEFAULT=0 shim/build.sh
#   build with the synthetic GPS feed off by default -- the right build for a
#   car with a real receiver. n200fs.conf's gps= line still overrides it.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-clang}"
OUT="$HERE/out/libn200fs.so"
TARGET="armv7-none-linux-androideabi"

while [ $# -gt 0 ]; do
    case "$1" in
        --cc)  CC="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,/^set -euo/p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

command -v "$CC" >/dev/null || { echo "error: no $CC on PATH" >&2; exit 1; }
command -v readelf >/dev/null || { echo "error: readelf not found" >&2; exit 1; }

WORK="$(mktemp -d -t n200fs.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$(dirname "$OUT")"

# ---------------------------------------------------------------- stub libs
# Each exports exactly the symbols the shim imports from it. Their bodies are
# never used: they exist so the link records DT_NEEDED and resolves the names.
emit_stub() {
    local soname="$1"; shift
    local src="$WORK/stub_${soname%.so}.c"
    : > "$src"
    for sym in "$@"; do
        echo "void $sym(void) {}" >> "$src"
    done
    "$CC" --target="$TARGET" -fPIC -nostdlib -shared -fuse-ld=lld -w \
          -Wl,-soname,"$soname" "$src" -o "$WORK/$soname"
}

emit_stub libdl.so  dlopen dlsym
emit_stub liblog.so __android_log_vprint
emit_stub libc.so   getenv memcpy memset usleep time pthread_create pipe fcntl fdopen

# ---------------------------------------------------------------- the shim
cat > "$WORK/export.map" <<'EOF'
{
  global:
    fopen; open; stat; access; opendir;
    mkdir; rmdir; unlink; remove; rename;
    utimes; statfs; system; strcasecmp;
    n200fs_note;
    __android_log_print;
    _Z22NaviGetNaviSDCardStatev;
    _Z22mdGetFirstLayerOnStatev;
    _Z21NaviGetMapInitialDonev;
    _Z17NaviChkFirstMountv;
    _Z28NaviGetUnloadNaviProcessFlagv;
    _Z27md_get_displayDriveShowModev;
    _Z27md_get_first_layer_on_statev;
    _Z27md_set_first_layer_on_statei;
    _Z13nvMsg_ReceiveiPFiiP8NV_PULSEEPFiiP13NV_MSG_HEADEREi;
    _Z6z_OpenPcP16SNavisFileHandle;
    _Z14zPSyncInfoLoadP16SNavisFileHandleP24SZFPartialSyncInfoHeader;
    _Z16zh_basefile_initPvPii;
    _Z18zh_maplist_db_openPcPP19CNavisSQLAccessInfoPP17CNavisSQLStmtInfo;
    _Z18zh_vertexfile_initPvPii;
    _ZN14NVDebugMessage8traceMsgEiPKcS1_S1_z;
    _ZN14NVDebugMessage8traceErrEiPKcS1_S1_z;
    _ZN14NVDebugMessage8traceWarEiPKcS1_S1_z;
  local: *;
};
EOF

"$CC" --target="$TARGET" -fPIC -Os -std=c99 \
      -DN200FS_GPS_DEFAULT="${GPS_DEFAULT:-1}" \
      -ffreestanding -fno-builtin -fno-stack-protector \
      -Wall -Wextra -Wno-unused-parameter \
      -c "$HERE/n200fs.c" -o "$WORK/n200fs.o"

"$CC" --target="$TARGET" -shared -nostdlib -fuse-ld=lld \
      -Wl,-soname,libn200fs.so \
      -Wl,--version-script="$WORK/export.map" \
      -Wl,--no-undefined \
      "$WORK/n200fs.o" "$WORK/libdl.so" "$WORK/liblog.so" "$WORK/libc.so" \
      -o "$WORK/libn200fs.so"

# ---------------------------------------------------------------- gates
fail=0
say() { printf '  %-5s %s\n' "$1" "$2"; [ "$1" = FAIL ] && fail=1 || true; }

echo "==> checking $(basename "$OUT")"

# 1. architecture
if readelf -h "$WORK/libn200fs.so" | grep -q "Machine:.*ARM" &&
   readelf -h "$WORK/libn200fs.so" | grep -q "Class:.*ELF32" &&
   readelf -h "$WORK/libn200fs.so" | grep -q "Type:.*DYN"; then
    say ok "ELF32 / DYN / ARM"
else
    say FAIL "not an ELF32 ARM shared object"
fi

# 2. SONAME
if readelf -d "$WORK/libn200fs.so" | grep -q "Library soname: \[libn200fs.so\]"; then
    say ok "SONAME is libn200fs.so"
else
    say FAIL "wrong or missing SONAME"
fi

# 3. exports are exactly the interposed set -- an extra export would be bound
#    by the app's libraries in place of libc's, which is how you break a
#    program you meant to fix.
want="_Z13nvMsg_ReceiveiPFiiP8NV_PULSEEPFiiP13NV_MSG_HEADEREi
_Z14zPSyncInfoLoadP16SNavisFileHandleP24SZFPartialSyncInfoHeader
_Z16zh_basefile_initPvPii
_Z17NaviChkFirstMountv
_Z18zh_maplist_db_openPcPP19CNavisSQLAccessInfoPP17CNavisSQLStmtInfo
_Z18zh_vertexfile_initPvPii
_Z21NaviGetMapInitialDonev
_Z22NaviGetNaviSDCardStatev
_Z22mdGetFirstLayerOnStatev
_Z27md_get_displayDriveShowModev
_Z27md_get_first_layer_on_statev
_Z27md_set_first_layer_on_statei
_Z28NaviGetUnloadNaviProcessFlagv
_Z6z_OpenPcP16SNavisFileHandle
_ZN14NVDebugMessage8traceErrEiPKcS1_S1_z
_ZN14NVDebugMessage8traceMsgEiPKcS1_S1_z
_ZN14NVDebugMessage8traceWarEiPKcS1_S1_z
__android_log_print
access
fopen
mkdir
n200fs_note
open
opendir
remove
rename
rmdir
stat
statfs
strcasecmp
system
unlink
utimes"
got=$(readelf --dyn-syms -W "$WORK/libn200fs.so" |
      awk '$4=="FUNC" && $5=="GLOBAL" && $7!="UND" {sub(/@.*/,"",$8); print $8}' |
      sort -u)
if [ "$want" = "$got" ]; then
    say ok "exports the 13 libc entry points, the 3 engine trace hooks, the 8 gate getters/setters, the nvMsg_Receive loop probe, the 5 map_initialize tracers and n200fs_note"
else
    say FAIL "export set differs"
    diff <(echo "$want") <(echo "$got") | sed 's/^/        /' || true
fi

# 4. dependencies: nothing but the three stable platform libraries
deps=$(readelf -d "$WORK/libn200fs.so" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | sort -u | tr '\n' ' ')
case " $deps " in
    *libc.so*) ;;
    *) say FAIL "libc.so is not a dependency: $deps" ;;
esac
if [ -z "$(echo "$deps" | tr ' ' '\n' | grep -vE '^(libc\.so|libdl\.so|liblog\.so)?$')" ]; then
    say ok "DT_NEEDED: $deps"
else
    say FAIL "unexpected dependency: $deps"
fi

# 5. no undefined symbol beyond what those three provide
und=$(readelf --dyn-syms -W "$WORK/libn200fs.so" | awk '$7=="UND" && $8!="" {print $8}' | sort -u)
unexpected=$(echo "$und" | grep -vE '^(dlopen|dlsym|getenv|__android_log_print|memcpy|memset|usleep|time|pthread_create|pipe|fcntl|fdopen)$' || true)
if [ -z "$unexpected" ]; then
    say ok "imports only: $(echo "$und" | tr '\n' ' ')"
else
    say FAIL "unexpected import(s): $(echo "$unexpected" | tr '\n' ' ')"
fi

[ "$fail" -eq 0 ] || { echo; echo "build rejected" >&2; exit 1; }

cp "$WORK/libn200fs.so" "$OUT"
echo
echo "==> $OUT ($(stat -c%s "$OUT") bytes)"
