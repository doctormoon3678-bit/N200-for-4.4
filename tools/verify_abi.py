#!/usr/bin/env python3
"""Acceptance test: prove an n200 APK has no private-platform ABI left.

This is the check that decides whether the patched APK has a chance of loading
on a stock phone. It answers two questions for every native library:

  1. Which dynamic symbols does the library import that NO library bundled in
     the APK provides? Those must come from the phone, and if any of them is a
     C++ symbol from the android:: namespace (or Skia, or ICU, or STLport) it is
     a private platform symbol with no ABI guarantee -- exactly what breaks the
     original APK on a Galaxy Note 4.

  2. Which DT_NEEDED entries are neither bundled in the APK nor part of the
     NDK-stable set? Those name private platform libraries.

On the original APK this reports libstagefright_dmb.so (144 android:: symbols)
and libskik.so (icu_51 / STLport internals), plus 17 private DT_NEEDED entries.
After the stub replaces libstagefright_dmb.so both lists should be empty.

Usage:
    verify_abi.py <apk-or-unpacked-dir> [--readelf PATH] [--verbose]

Exit status is 0 only when nothing private remains.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

# Libraries the NDK guarantees across Android versions. Everything else that a
# library pulls in from /system/lib is a private platform dependency.
NDK_STABLE = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libstdc++.so",
    "libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so",
    "libGLESv3.so", "libjnigraphics.so", "libOpenSLES.so", "libOpenMAXAL.so",
}

# A C++ mangled symbol in one of these namespaces/classes, coming from outside
# the APK, means a private platform ABI dependency.
PRIVATE_PATTERNS = [
    (re.compile(r"^_ZN?K?[0-9]*7android"), "android:: (libstagefright/libmedia/libgui/libutils/...)"),
    (re.compile(r"^_ZN?K?[0-9]*6icu_[0-9]+"), "icu_NN:: (libicuuc/libicui18n)"),
    (re.compile(r"^_ZT?V?N?[0-9]*Sk"), "Skia (libskia)"),
    (re.compile(r"^_ZN?K?St[0-9]*1?2?__node_alloc"), "STLport internals (libstlport)"),
    (re.compile(r"^_ZSt24__stl_throw"), "STLport internals (libstlport)"),
]


def find_readelf(explicit: str | None) -> str:
    if explicit:
        return explicit
    for name in ("readelf", "llvm-readelf", "eu-readelf"):
        p = shutil.which(name)
        if p:
            return p
    print("error: no readelf found (install binutils, or pass --readelf)",
          file=sys.stderr)
    sys.exit(2)


def run(readelf: str, *args: str) -> str:
    try:
        return subprocess.run([readelf, *args], capture_output=True, text=True,
                              check=False).stdout
    except OSError as exc:
        print(f"error: cannot run {readelf}: {exc}", file=sys.stderr)
        sys.exit(2)


def dyn_syms(readelf: str, path: pathlib.Path) -> tuple[set[str], set[str]]:
    """Return (defined, undefined) dynamic symbol names."""
    defined: set[str] = set()
    undef: set[str] = set()
    for line in run(readelf, "-sW", "--dyn-syms", str(path)).splitlines():
        f = line.split()
        # Num: Value Size Type Bind Vis Ndx Name
        if len(f) < 8 or not f[0].endswith(":"):
            continue
        bind, ndx, name = f[4], f[6], f[7]
        name = name.split("@", 1)[0]
        if not name:
            continue
        if ndx == "UND":
            undef.add(name)
        elif bind in ("GLOBAL", "WEAK"):
            defined.add(name)
    return defined, undef


def machine(readelf: str, path: pathlib.Path) -> str:
    for line in run(readelf, "-h", str(path)).splitlines():
        if "Machine:" in line:
            return line.split("Machine:", 1)[1].strip()
    return "?"


def needed(readelf: str, path: pathlib.Path) -> list[str]:
    out = []
    for line in run(readelf, "-d", str(path)).splitlines():
        if "NEEDED" in line:
            m = re.search(r"\[(.+?)\]", line)
            if m:
                out.append(m.group(1))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="APK file or unpacked APK directory")
    ap.add_argument("--readelf")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="list every offending symbol, not just a sample")
    args = ap.parse_args()

    readelf = find_readelf(args.readelf)
    target = pathlib.Path(args.target)
    tmp = None

    if target.is_file():
        tmp = tempfile.mkdtemp(prefix="n200verify.")
        with zipfile.ZipFile(target) as z:
            z.extractall(tmp)
        root = pathlib.Path(tmp)
    elif target.is_dir():
        root = target
    else:
        print(f"error: {target} not found", file=sys.stderr)
        return 2

    try:
        libs = sorted(root.glob("lib/*/*.so"))
        if not libs:
            print(f"error: no lib/*/*.so inside {target}", file=sys.stderr)
            return 2

        bundled_names = {p.name for p in libs}
        bundled_defined: set[str] = set()
        syms = {}
        for p in libs:
            d, u = dyn_syms(readelf, p)
            syms[p] = (d, u)
            bundled_defined |= d

        print(f"==> {len(libs)} native libraries, "
              f"{len(bundled_defined)} symbols defined inside the APK\n")

        # Every library in lib/armeabi must be ARM. Catches the easy mistake of
        # building the stub for the host or for the wrong APP_ABI.
        print("--- architecture ---")
        wrong_arch = []
        for p in libs:
            m = machine(readelf, p)
            if "ARM" not in m.upper() or "AARCH64" in m.upper():
                wrong_arch.append((p.name, m))
        if wrong_arch:
            for name, m in wrong_arch:
                print(f"  {name}: {m}   <-- must be 32-bit ARM")
        else:
            print(f"  all {len(libs)} libraries are 32-bit ARM")
        print()

        bad_syms = 0
        print("--- imported symbols nothing in the APK provides, that are private ---")
        for p in libs:
            _, u = syms[p]
            external = u - bundled_defined
            offenders: dict[str, list[str]] = {}
            for name in sorted(external):
                for rx, label in PRIVATE_PATTERNS:
                    if rx.match(name):
                        offenders.setdefault(label, []).append(name)
                        break
            if offenders:
                total = sum(len(v) for v in offenders.values())
                bad_syms += total
                print(f"  {p.name}: {total}")
                for label, names in sorted(offenders.items()):
                    print(f"      {len(names):4d}  {label}")
                    show = names if args.verbose else names[:3]
                    for n in show:
                        print(f"            {n}")
                    if not args.verbose and len(names) > 3:
                        print(f"            ... and {len(names) - 3} more "
                              f"(-v to list)")
        if bad_syms == 0:
            print("  none")

        # A DT_NEEDED entry containing a slash is a path, not a soname: the
        # KitKat linker opens it relative to the process cwd ("/" for an app).
        # libNV_GUI.so and libNV_KORTPEG.so both carry
        # "./obj/local/armeabi/libdmb_cont.so", a leftover build path caused by
        # libdmb_cont.so having no SONAME. It happens to resolve only because
        # find_loaded_library() matches on basename and
        # LocalServiceActivities.<clinit> loads "dmb_cont" earlier in its list.
        # Harmless today, fatal the moment that load order changes.
        print("\n--- DT_NEEDED entries that are paths, not sonames (latent) ---")
        path_needed = set()
        for p in libs:
            for n in needed(readelf, p):
                if "/" in n:
                    path_needed.add((n, p.name))
        if path_needed:
            for n, by in sorted(path_needed):
                base = n.rsplit("/", 1)[-1]
                state = ("resolves only via basename match on an already-loaded "
                         f"{base}") if base in bundled_names else "WILL NOT RESOLVE"
                print(f"  {n}   (needed by {by}) -- {state}")
        else:
            print("  none")

        print("\n--- DT_NEEDED entries that are neither bundled nor NDK-stable ---")
        bad_needed = set()
        for p in libs:
            for n in needed(readelf, p):
                base = n.rsplit("/", 1)[-1]
                if base in bundled_names or base in NDK_STABLE:
                    continue
                if base == p.name:        # own SONAME echoed back
                    continue
                bad_needed.add((n, p.name))
        if bad_needed:
            for n, by in sorted(bad_needed):
                print(f"  {n}   (needed by {by})")
        else:
            print("  none")

        print()
        if bad_syms or bad_needed or wrong_arch:
            parts = []
            if bad_syms:
                parts.append(f"{bad_syms} private symbols")
            if bad_needed:
                parts.append(f"{len(bad_needed)} private library dependencies")
            if wrong_arch:
                parts.append(f"{len(wrong_arch)} libraries of the wrong architecture")
            print(f"RESULT: FAIL -- {', '.join(parts)}.")
            print("        This APK still binds to platform-internal ABI and can")
            print("        fail to dlopen on any device but the original one.")
            return 1
        print("RESULT: PASS -- nothing outside the NDK-stable ABI remains.")
        return 0
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
