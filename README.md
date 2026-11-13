# N200 for Android 4.4

Makes `n200_signed.apk` (`com.npnd.n200`, a Korean car head-unit navigation app)
run on a stock Android 4.4 phone, by repointing its hard-coded data root and, if
the device needs it, removing its dependency on platform-internal ABI.

The APK is a head-unit ROM app: 30 native libraries, 48 MB, `armeabi`, signed
with the public AOSP **testkey**, `debuggable=true`, `minSdk=targetSdk=19`.

## What actually crashes

From a tombstone on `samsung/hltelgt:4.4.2/KOT49H` (SM-N900L, Dalvik):

```
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0000000c
r0 00000000   sl 00000000

#00 pc 00026e64 /system/lib/libc.so (__vfprintf+27)
#01 pc 00028049 /system/lib/libc.so (vfprintf+32)
#02 pc 000258e9 /system/lib/libc.so (fprintf+12)
#03 pc 00009fd8 /data/app-lib/com.npnd.n200-2/libNV_CNS2K.so (startMain(_JNIEnv*, _jobject*)+1496)
#04 pc 0000a228 ... (Java_com_npnd_n200_serv_LocalServiceCNS2K_StartCNS2KMain+348)
```

`fprintf` was handed a NULL `FILE*`. That is not a guess — decoding the bytes in
the tombstone's `code around pc` pins it:

```
__vfprintf:  E92D 4FF0   PUSH.W {r4-r11,lr}
             ...
             4682        MOV r10, r0          ; r10 = the FILE* argument
     pc ->   F8BA 100C   LDR.W r1, [r10, #12] ; reads fp->_flags
```

`r0` is 0, so `r10` is 0, so the load touches `0x0000000c` — exactly the reported
fault address. Offset 12 is `_flags` in bionic's `struct __sFILE`.

So `libNV_CNS2K.so` calls `fopen()` on a path that does not exist, never checks
the result, and `fprintf`s into it. Every file `startMain` touches lives under
the data root the app hard-codes:

```
/mnt/extSdCard///Navkingdom/LoggerServerPID            <- written with fprintf
/mnt/extSdCard///Navkingdom/meminfo.txt
/mnt/extSdCard///Navkingdom/Navi/CONF/naviConfig_N200.txt
/mnt/extSdCard///Navkingdom/Common/DB_MAP/NF60_NPND_Korea.sti
%s/%s_%s.txt                                           <- NVSLOG, written with fprintf
```

`/mnt/extSdCard` does not exist on a Samsung KitKat device — the SD card is at
`/storage/extSdCard` — and since KitKat `WRITE_EXTERNAL_STORAGE` no longer lets a
third-party app write anywhere on secondary storage outside
`Android/data/<package>/`, so these writes would fail even if the directory were
there. The app hard-codes that root in **288 places across 13 files**.

**This is what `tools/patch_paths.py` fixes, and it is the fix to try first.**

## The other hazard: private platform ABI

Note what the tombstone also proves: the process reached a JNI call from
`LocalServiceCNS2K`, which means `LocalServiceActivities.<clinit>` had already
loaded **all 25 native libraries**, `libstagefright_dmb.so` included. On that
device the private platform symbols below *do* resolve, and this part of the
project is not needed. It is kept because it is device-dependent — a different
4.4 build can refuse them — and because the audit tooling is what established
that only two libraries are exposed at all.

The launcher activity loads 25 libraries in one static initializer, before
anything is drawn, so any single `dlopen` failure there kills the process before
a window appears:

```
  com/npnd/n200/LocalServiceActivities-><clinit>
      1  gnustl_shared
      ...
     10  dmb_cont         RISK  via libstagefright_dmb.so: 140 x android::
                                via libskik.so: 8 x icu_51::, 4 x STLport internals
     12  HNcommonlib      RISK  (same chain)
     16  GUI_WIDGET       RISK  (same chain)
     17  NV_GUI           RISK  (same chain)
```

Load #10 is the exposed one:

```
libdmb_cont.so -> libstagefright_dmb.so -> libstagefright.so, libstagefright_foundation.so,
                                           libmedia.so, libgui.so, libui.so, libutils.so,
                                           libbinder.so, libandroid_runtime.so, libcutils.so
                                        -> libskik.so (SONAME=libskia.so)
                                           -> libstlport.so, libft2.so, libicuuc.so,
                                              libicui18n.so, libexpat.so, libpng.so
```

`libstagefright_dmb.so` imports ~144 *private* C++ symbols — `android::AudioTrack`,
`android::MediaCodec`, `android::SurfaceComposerClient`, `android::GraphicBufferMapper`,
`android::TextLayoutEngine`, `android::RefBase`/`VectorImpl`/`String8` and so on.
For example:

```cpp
android::AudioTrack::AudioTrack(audio_stream_type_t, uint32_t, audio_format_t, uint32_t,
                                int, audio_output_flags_t, void(*)(int,void*,void*), void*,
                                int, int, transfer_type, audio_offload_info_t const*, int)
android::MediaCodec::configure(sp<AMessage> const&, sp<Surface> const&,
                               sp<ICrypto> const&, uint32_t)
```

None of that is a stable ABI. Those signatures change between 4.4.x patch levels
and between OEM builds. They happen to match on the `hltelgt:4.4.2/KOT49H` build
above; there is no guarantee they match on the next one.

**The good news**, from auditing all 30 libraries: only **2** of them touch
private ABI (`libstagefright_dmb.so` and `libskik.so`), the other 28 use nothing
but the NDK-stable set — and across the whole APK the app only ever calls **16**
of `libstagefright_dmb.so`'s 1458 exports, all on two classes:

| class | purpose | called by |
|---|---|---|
| `android::DMBPlayerClass` | terrestrial DMB TV tuner/player | `libdmb_cont.so`, `libNV_GUI.so` |
| `android::TTSPlayerClass` | PCM playback sink for voice guidance | `libHNcommonlib.so`, `libGUI_WIDGET.so` |

### Swapping it out, when you need to

Replace `libstagefright_dmb.so` with a 16-symbol stub. That single swap drops the
entire private surface: `libskik.so`, `libstlport.so`, `libft2.so`, `libicuuc.so`,
`libicui18n.so`, `libexpat.so`, `libpng.so`, `libjpeg.so`, `libcutils.so`,
`libutils.so`, `libbinder.so`, `libmedia.so`, `libgui.so`, `libui.so`,
`libstagefright.so`, `libstagefright_foundation.so`, `libandroid_runtime.so` and
`libcorkscrew.so` all stop being referenced.

Measured on the real APK, before and after the swap:

| | original | with stub |
|---|---|---|
| private platform symbols | 152 | **0** |
| private platform library deps | 20 | **0** |
| libraries in `<clinit>` at risk | 14 of 25 | **0** |

### What it costs

Do not reach for the stub unless the device actually refuses the private
symbols, because the swap is not free:

* **DMB TV does not work.** Irrelevant on a phone — there is no DMB tuner.
* **Voice guidance is silent.** Note that only the *playback sink* is stubbed:
  the Korean TTS synthesis engine `libpowertts.so` needs nothing but
  libc/libm/liblog and is fully portable. Restoring voice guidance means
  reimplementing `TTSPlayerClass` on OpenSL ES — a contained follow-up, not a
  rewrite.

## Usage

Requirements: a JDK with `jarsigner`/`keytool`, `zip`/`unzip`, Python 3.9+, and
`readelf`. Only the optional stub needs an NDK.

**Start here.** Repoint the data root and leave the vendor libraries alone —
this is the fix for the crash above, and it keeps DMB and voice guidance:

```sh
# put the original APK here (it is gitignored)
cp /path/to/n200_signed.apk apk/n200_signed.apk

tools/repack.sh --keep-dmb        # -> out/n200-note4.apk
```

Only if the device then fails to load the libraries — a logcat line reading
`dlopen failed: cannot locate symbol "_ZN7android…" referenced by
"libstagefright_dmb.so"` — build the stub and swap it in, accepting the cost
listed above:

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r21e   # or use the CI artifact
stub/build.sh
tools/repack.sh --strip-dead
```

Install. The signature changes from the original AOSP testkey, so an in-place
upgrade is rejected:

```sh
adb uninstall com.npnd.n200
adb install out/n200-note4.apk
adb shell mkdir -p /sdcard/n200dt
adb push <your Navkingdom tree> /sdcard/n200dt/Navkingdom
```

Watch it start:

```sh
adb logcat -c
adb shell am start -n com.npnd.n200/.LocalServiceActivities
adb logcat -d | grep -iE "AndroidRuntime|linker|dlopen|UnsatisfiedLink|n200"
```

### When it still misbehaves

Build the tracing stub and watch what the navigation engine asks of DMB/TTS:

```sh
STUB_TRACE=1 stub/build.sh && tools/repack.sh --strip-dead
adb logcat -s n200stub
```

If the engine calls `TTSPlayerClass::start()` and then stalls, it is waiting for
a "playback finished" notification a silent stub never delivers. Make the stub
report failure instead, so the engine takes its own error path:

```sh
STUB_START_FAILS=1 stub/build.sh && tools/repack.sh --strip-dead
```

## Building on GitHub Actions

`.github/workflows/build-stub.yml` builds the stub and runs every gate, so you
do not need an NDK locally. It runs on pushes that touch `stub/`, and can be
started by hand from the Actions tab (**build stub** → *Run workflow*), where
`ndk_version` defaults to `r21e`.

One run produces all three variants in a single artifact — `default`, `trace`
and `start-fails` — so debugging on the device does not need another run:

```sh
gh run download --name libstagefright_dmb-stub-r21e
tools/repack.sh --strip-dead --stub default/libstagefright_dmb.so
```

The job log also prints the finished library's `DT_NEEDED` list, SONAME,
exported symbols and ARM build attributes, which is the evidence that the swap
is sound.

**Repacking is deliberately not in CI.** This repository is public, and so are
its Actions artifacts and logs. Committing `n200_signed.apk`, fetching it during
a run, or uploading a repacked copy would publish a commercial navigation app
and its map engine to anyone who finds the repo. So CI stops at the stub — the
one piece that is this project's own code — and `tools/repack.sh` stays local.
Make the repository private and the repack job becomes safe to add.

## Tools

| | |
|---|---|
| `stub/jni/stub_players.cpp` | the 16 stub symbols, with the ABI reasoning in the header comment |
| `stub/build.sh` | builds and gates the stub (symbol set, SONAME, dependencies) |
| `tools/audit.py` | prints the `System.loadLibrary` sequence from `classes.dex` and marks which loads reach private ABI — tells you *where* startup dies |
| `tools/verify_abi.py` | acceptance test: private symbols, private `DT_NEEDED`, architecture. Exit 0 only when clean |
| `tools/check_mangling.sh` | fast gate: host-compiles the stub and checks the mangled names, no NDK needed |
| `tools/patch_paths.py` | rewrites the hard-coded data root in the `.so` files and `classes.dex` |
| `tools/repack.sh` | the whole pipeline, with the audit as a release gate |

Both `audit.py` and `verify_abi.py` take either an APK or an unpacked directory,
so they work on the original too:

```sh
python3 tools/audit.py apk/n200_signed.apk        # -> first failure at load #10
python3 tools/verify_abi.py apk/n200_signed.apk   # -> FAIL, 152 private symbols
```

## Verified, and not

Checked directly, against the real APK:

* The stub source mangles to **exactly** the 16 required symbols, no omissions.
  (Itanium mangling is architecture-independent, so this was confirmed with a
  host `g++` build and `readelf`.) The two extra `C2` base-object constructor
  aliases GCC emits are harmless.
* No vtable, typeinfo or destructor of either stubbed class is imported by any
  of the 30 libraries — which is what makes a non-polymorphic, no-data-member
  stub safe. The ctor bodies deliberately write nothing: the callers already
  have `sizeof()` baked in from their own build.
* Path patching is byte-for-byte equal length, so no file offset moves: ELF
  headers, relocations and symbol tables stay valid, and the dex string table's
  ULEB128 length prefixes stay correct. The dex adler32 checksum and SHA-1
  signature are recomputed, and both verify afterwards.
* Repack and signing produce an APK whose signature verifies, with **SHA-256**
  digests pinned. This matters: left to itself, JDK 21 writes SHA-384, which
  API 19 is not documented to accept. SHA1 is not an option either — recent
  JDKs refuse to emit it.
* Substituting the stub removes all 152 private symbols and all 20 private
  library dependencies, and clears every RISK from the load sequence.

Confirmed by an actual ARM build in CI (run 2, NDK r21e):

* The library really is `ELF32 / DYN / Machine: ARM`, built `Tag_CPU_arch: v7`,
  `Tag_FP_arch: VFPv3` — matching the app's own libraries — with SONAME
  `libstagefright_dmb.so` and `DT_NEEDED` of only libdl, liblog, libc, libm and
  libstdc++. 13904 bytes against the original's 2349248.
* All four gates pass on each of the three variants: required symbols present,
  no unexpected exports, SONAME intact, NDK-stable dependencies only.
* The first CI build earned its keep: it caught the stub re-exporting the
  statically linked libunwind (`_Unwind_*`, `__aeabi_unwind_cpp_pr0/1/2`,
  `unw_*`). Because the KitKat linker searches the requesting library's own
  `DT_NEEDED` list first, and three of the app's libraries list
  `libstagefright_dmb.so`, they would have bound their unwinder to the stub's
  copy instead of libgnustl_shared.so's — breaking exception handling in the
  navigation engine. `stub/jni/stub.map` now pins the exported set to exactly
  the 16 symbols (plus the two C2 aliases), and both build gates fail on any
  stray export. The host check applies the same version script, so it would
  catch a regression without an NDK.

Not verified, and honestly so:

Settled by a device tombstone (`hltelgt:4.4.2/KOT49H`):

* The crash is `fprintf` on a NULL `FILE*` inside
  `libNV_CNS2K.so startMain()`, reached through
  `Java_com_npnd_n200_serv_LocalServiceCNS2K_StartCNS2KMain`. Decoding the
  tombstone's own `code around pc` confirms the mechanism rather than inferring
  it: `MOV r10, r0` then `LDR.W r1, [r10, #12]`, with `r0`/`sl` both 0 and the
  fault address `0x0000000c` — `fp->_flags` through a null pointer.
* **It corrected the original diagnosis.** All 25 libraries had already loaded
  by then, so the private platform symbols resolve on that build and the stub is
  not what was needed. The data root is. Leading with the private-ABI theory was
  wrong, and the repack default changed accordingly: `--keep-dmb` no longer
  treats the ABI audit as a release gate.

Still not verified:

* **Which file the failing `fopen` names.** `startMain` references the
  `LoggerServerPID` file, `meminfo.txt`, `naviConfig_N200.txt`, the `.sti` map
  files and an `NVSLOG` `%s/%s_%s.txt` pattern; the two written with `fprintf`
  are the PID file and the log. Pinning it exactly needs disassembly, and this
  environment has no ARM disassembler and no network to fetch one. It does not
  change the fix: all of them sit under the same missing data root.
* **Whether repointing the root is sufficient on its own.** `fopen` will keep
  returning NULL — and the app will keep crashing in the same place — unless the
  `Navkingdom` data tree actually exists under the new root and is writable.
* **No repack has been done with the CI-built ARM stub.** The repack pipeline
  was exercised end to end with the original library in place (`--keep-dmb`),
  and the ABI end state was measured by substituting a host build, but the two
  halves have not been put together in one run. Doing that is the first thing
  `tools/repack.sh --strip-dead` will tell you.
* Whether the navigation engine tolerates a silent TTS sink. See the tracing
  build above.

## Known remaining obstacles

Beyond the data root and the private ABI:

* `SET_TIME` and `SET_PREFERRED_APPLICATIONS` are signature/system permissions —
  the app was a platform-signed system app. Installed normally they are not
  granted, so those calls raise `SecurityException`.
* Other head-unit-only paths survive: `/mnt/hdd/nav1/map/...`,
  `/storage/usb0/NVSLOG.CONF`, `/sys/devices/virtual/switch/reverse/state`
  (reverse-gear detection). They appear to be fallbacks and logging, not startup
  requirements, but they will never resolve.
* `libHMI_BASE.so` and `libNV_MD.so` need `libGLESv3.so`. Present on a Note 4
  (Adreno 420 / Mali-T760 both do GLES 3.x), so expected to be fine.
* `libNV_GUI.so` and `libNV_KORTPEG.so` carry a `DT_NEEDED` of
  `./obj/local/armeabi/libdmb_cont.so` — a leftover build path, caused by
  `libdmb_cont.so` having no SONAME. It resolves only because the KitKat linker
  matches already-loaded libraries by **basename** and `<clinit>` loads
  `dmb_cont` at #10, before `NV_GUI` at #17. Harmless today, fatal if that order
  ever changes. `tools/verify_abi.py` reports it as latent.
* The manifest is malformed — two `<application>` elements, with permissions
  declared after the first one closes, and an `android.app.NativeActivity`
  entry whose `libn200.so` is not in the APK. KitKat's non-rigid package parser
  warns and moves on, and nothing references that activity, so none of it
  blocks installation or startup.
