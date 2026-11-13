#!/usr/bin/env python3
"""Rebuild an APK zip with the same entry metadata the original carried.

Info-ZIP's `zip` stamps entries as Unix (create_system 3) and stores the file
mode in external_attr. The original APK, written by the Android build tools, has
create_system 0 and external_attr 0 everywhere. Android does not document caring
about either, but when an install is failing with no error code to go on there
is no reason to keep gratuitous differences from a package that does install.

Entry order and per-entry compression method are copied from the original too,
so the only thing that differs between the two archives is the bytes we meant to
change. Files not present in the original (there should be none besides
META-INF, which the signer adds afterwards) are appended, deflated.

Usage:
    rezip.py <unpacked-dir> <original.apk> <output.zip>
"""

import pathlib
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    root, original, out = (pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]),
                           pathlib.Path(sys.argv[3]))
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 1
    if not original.is_file():
        print(f"error: {original} not found", file=sys.stderr)
        return 1

    with zipfile.ZipFile(original) as z:
        order = [i.filename for i in z.infolist() if not i.is_dir()]
        method = {i.filename: i.compress_type for i in z.infolist()}
        date = {i.filename: i.date_time for i in z.infolist()}

    on_disk = {
        str(p.relative_to(root)).replace("\\", "/")
        for p in root.rglob("*") if p.is_file()
    }
    # META-INF is dropped before repacking and re-created by the signer.
    ordered = [n for n in order if n in on_disk and not n.startswith("META-INF/")]
    extra = sorted(on_disk - set(order))
    if extra:
        print(f"    note: {len(extra)} file(s) not in the original, appended: "
              f"{', '.join(extra[:5])}")

    with zipfile.ZipFile(out, "w") as z:
        for name in ordered + extra:
            src = root / name
            info = zipfile.ZipInfo(name, date_time=date.get(name, (1980, 1, 1, 0, 0, 0)))
            info.compress_type = method.get(name, zipfile.ZIP_DEFLATED)
            # Match the original: MS-DOS, no mode bits.
            info.create_system = 0
            info.external_attr = 0
            with open(src, "rb") as fh:
                z.writestr(info, fh.read())
    print(f"    rezipped {len(ordered) + len(extra)} entries, "
          f"metadata matched to the original")
    return 0


if __name__ == "__main__":
    sys.exit(main())
