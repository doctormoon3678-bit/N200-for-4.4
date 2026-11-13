# N200 for Android 4.4

Makes `n200_signed.apk` (`com.npnd.n200`, a Korean car head-unit navigation app)
loadable on a stock Android 4.4 phone such as the Galaxy Note 4, by removing its
dependency on platform-internal ABI and repointing its hard-coded data root.

## The problem

The APK is a head-unit ROM app: 30 native libraries, 48 MB, `armeabi`, signed
with the public AOSP **testkey**, `debuggable=true`, `minSdk=targetSdk=19`.

`com.npnd.n200.LocalServiceActivities` — the launcher activity — loads **25
native libraries in its static initializer**. All of them, before anything is
drawn. So a single failing `dlopen` kills the process the moment the launcher
class is touched, which is what "the app just bounces" looks like:

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

Load #10 is the problem:

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
and between OEM builds, and Samsung's media stack is heavily modified. The
symbols resolved on the head unit the app was built for; on a Note 4 they do not.

**The good news**, from auditing all 30 libraries: only **2** of them touch
private ABI (`libstagefright_dmb.so` and `libskik.so`), the other 28 use nothing
but the NDK-stable set — and across the whole APK the app only ever calls **16**
of `libstagefright_dmb.so`'s 1458 exports, all on two classes:

| class | purpose | called by |
|---|---|---|
| `android::DMBPlayerClass` | terrestrial DMB TV tuner/player | `libdmb_cont.so`, `libNV_GUI.so` |
| `android::TTSPlayerClass` | PCM playback sink for voice guidance | `libHNcommonlib.so`, `libGUI_WIDGET.so` |

## The fix

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

Then repoint the data root, because the app hard-codes `/mnt/extSdCard` in 288
places across 13 files. On a Note 4 the SD card is at `/storage/extSdCard` and
`/mnt/extSdCard` does not exist; and since KitKat, `WRITE_EXTERNAL_STORAGE` no
longer lets a third-party app write anywhere on secondary storage outside
`Android/data/<package>/`, so the `Persistence/*.bak` writes would fail anyway.
The default replacement is `/sdcard/n200dt` on primary storage.

### What it costs

* **DMB TV does not work.** Irrelevant on a phone — there is no DMB tuner.
* **Voice guidance is silent.** Note that only the *playback sink* is stubbed:
  the Korean TTS synthesis engine `libpowertts.so` needs nothing but
  libc/libm/liblog and is fully portable. Restoring voice guidance means
  reimplementing `TTSPlayerClass` on OpenSL ES — a contained follow-up, not a
  rewrite.

## Usage

Requirements: an Android NDK that still supports `android-19` (r21e is a good
last one; r27+ has dropped it), a JDK with `jarsigner`/`keytool`, `zip`/`unzip`,
Python 3.9+, and `readelf` (or the NDK's bundled `llvm-readelf`).

```sh
# 0. put the original APK here (it is gitignored)
cp /path/to/n200_signed.apk apk/n200_signed.apk

# 1. build the stub -- refuses to finish unless it exports exactly the 16
#    symbols in stub/expected_symbols.txt, keeps SONAME libstagefright_dmb.so,
#    and depends on nothing outside the NDK-stable set
export ANDROID_NDK_HOME=/path/to/android-ndk-r21e
stub/build.sh

# 2. repack: swap the stub in, repoint the data root, re-sign, re-audit
tools/repack.sh --strip-dead

# -> out/n200-note4.apk
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

Not verified, and honestly so:

* **Nothing has run on a device.** The conclusion that the original APK fails at
  load #10 is inferred from its dependency graph, not observed in a logcat. The
  logcat command above settles it; `cannot locate symbol "_ZN7android…"
  referenced by "libstagefright_dmb.so"` confirms it.
* **The stub has never been compiled for ARM here** — this environment has no
  NDK and no network to fetch one. `stub/build.sh` and `tools/verify_abi.py` are
  the gates that confirm it on your machine.
* Whether the navigation engine tolerates a silent TTS sink. See the tracing
  build above.

## Known remaining obstacles

Beyond the two this repo addresses:

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
