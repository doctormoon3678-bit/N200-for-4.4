# N200 for Android 4.4

Makes `n200_signed.apk` (`com.npnd.n200`, a Korean car head-unit navigation app)
run on a stock Android 4.4 phone, by redirecting its hard-coded data root and,
if the device needs it, removing its dependency on platform-internal ABI.

**Map data on the external SD card, writes on internal storage:**

```sh
shim/build.sh && stub/build.sh           # clang; no NDK needed
tools/repack.sh --sdcard --strip-dead    # -> out/n200-note4.apk
```

Put the `Navkingdom` tree anywhere on the card; the app finds it at startup.
That is [the section below](#map-data-on-the-sd-card). The rest of this
file explains why it needs doing that way.

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

`/mnt/extSdCard` is not where KitKat mounts a card — Samsung puts it at
`/storage/extSdCard`, other vendors at `/storage/sdcard1`, `/storage/ext_sd`,
`/storage/external_SD` — and since KitKat `WRITE_EXTERNAL_STORAGE` no longer
lets a third-party app write anywhere on secondary storage outside
`Android/data/<package>/`, so these writes would fail even if the directory were
there. The app hard-codes that root in **288 places across 13 files**.

### Why rewriting the strings cannot fix it

The obvious repair is to rewrite that root in the `.so` files, and
`tools/patch_paths.py` does exactly that. But a replacement has to be **no
longer** than the string it replaces, or every file offset in the ELF moves and
each section header, relocation and symbol table entry goes stale. `/mnt/extSdCard`
is 14 bytes. `/storage/extSdCard` is 18. There is no 14-byte path to a KitKat
SD card, and there is no single right answer to compile in either, because the
mount point differs per vendor.

So a string rewrite can only reach **primary** storage — `/sdcard/n200dt`, which
happens to be 14 bytes. That works, and it is what `tools/repack.sh` does by
default, but it forces the whole map database onto internal storage. A Korea map
set is several gigabytes, which is exactly what the card was for.

**`--sdcard` solves it a different way: it stops rewriting the paths and
redirects them when they are used.** See
[Map data on the SD card](#map-data-on-the-sd-card).

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

### The adapter nobody mentioned: libtextlayoutshim.so

`libstagefright_dmb.so` is not the only private-ABI consumer, and the other one
explains why this APK is stranger than it looks.

`libtextlayoutshim.so` is not part of the app. Someone built it to adapt the
head unit's `libstagefright_dmb.so` to a **newer** Android than it was compiled
for. It exports the 4.4-era platform signatures and implements them with the
5.x ones:

| exports (what `libstagefright_dmb.so` wants) | imports (what it calls) |
|---|---|
| `AudioTrack::AudioTrack(..., transfer_type, const audio_offload_info_t*, int)` | the same plus `int, const audio_attributes_t*` |
| `AudioTrack::write(const void*, size_t)` | `write(const void*, size_t, bool)` |
| `AudioTrack::getPosition(uint32_t*) const` | `getPosition(uint32_t*)`, non-const |
| `MediaCodec::CreateByType(..., bool)` | `CreateByType(..., bool, int*)` |

So this APK had already been adapted once, for Android 5.x. Those four imports
are private platform ABI in their 5.x spelling, reached at load #10, and they
are a `dlopen` failure on a platform that is not the one it was built against.

Every export it exists for is imported by `libstagefright_dmb.so` and by
nothing else, so once that is stubbed the adapter is dead weight — but it
cannot just be deleted, because `libNV_CNS2K.so` lists it in `DT_NEEDED` and
imports exactly one symbol from it: `forc`. Eighteen bytes of Thumb-2 that
return `-1`, reproduced exactly in `stub/jni/stub_textlayoutshim.c` rather than
guessed at.

Replacing it is what finally removes the private ABI, and with it the last
reason to load `libskik.so`.

### Swapping it out, when you need to

Replace `libstagefright_dmb.so` with a 16-symbol stub, and `libtextlayoutshim.so`
with a 2-symbol one. Together they drop the entire private surface: `libskik.so`, `libstlport.so`, `libft2.so`, `libicuuc.so`,
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

## Running without a GPS fix

The engine will not leave its "preparing navigation" screen until the position
module has a fix. Its own log says so, 1170 identical lines of it:

```
[PREDICT_LOG] 0 0 0 0 1 0 0
```

That is the dead-reckoning loop running normally on all-zero input. Position
reaches it as raw NMEA through a FIFO:

```
Android GPS -> GpsBridge (Java) -> /data/data/com.npnd.n200/files/gpsfifo
            -> libNV_UART.so -> libNV_POS.so
```

`GpsBridge` is not part of the original app either — someone added it, like
`libtextlayoutshim.so`. It forwards `onNmeaReceived()` only; its
`onLocationChanged()` is an empty method. **That is why a mock-location or
"fake GPS" app cannot drive this**: mock providers deliver `Location` objects,
never NMEA sentences. And indoors, with no real fix, nothing is written at all
and the app cannot start.

So `libn200fs.so` writes the NMEA itself — it is already in the process, and a
FIFO does not care which thread fills it. A stationary fix is sent at 1 Hz:

```
$GPRMC,122225.00,A,3510.7760,N,12904.5360,E,0.00,0.00,131126,,,A*53
$GPGGA,122225.00,3510.7760,N,12904.5360,E,1,10,0.9,50.0,M,20.0,M,,*58
$GPGSA,A,3,01,02,03,04,05,06,07,08,09,10,,,1.6,0.9,1.3*3E
$GPGSV,1,1,10,01,60,045,45,02,55,135,44,03,50,225,43,04,45,315,42*7A
$GPVTG,0.00,T,,M,0.00,N,0.00,K,A*3D
```

`libNV_UART.so` sets the requirements with two of its own messages:
`NMEA Data Read Fail because of no GPRMC Message` (GPRMC is mandatory) and
`NMEA checksum err: %s` (checksums are verified). Both are satisfied, and the
self-test checks every sentence's checksum, the valid flag and the coordinate
conversion rather than assuming.

It also creates the FIFO with `mkfifo(3)` if `GpsBridge` has not. The bridge
shells out to `mknod` / `toolbox mknod`, which is not present on every build.

Default position is Seoul City Hall. In `n200fs.conf`:

```
gps=35.1796,129.0756     # somewhere else, decimal degrees
gps=off                  # real GPS only
```

**Turn it off for real driving.** The feed does not stop when a real fix
arrives — the bridge and this would both be writing to the same FIFO, and the
engine would see the position jump between the two. The synthetic feed is for
getting the app running, and for indoors.

No floating point is involved: this library links `-nostdlib`, so there is no
libgcc to provide soft-float or division helpers. Coordinates are held in
microdegrees and formatted with integer arithmetic, and the build gate fails
on any unexpected import, which is what keeps that true.

## The Java card-mounted test

Independent of where the data lives, and the reason the app can reach its
"preparing navigation" screen and then sit there for ever.

The engine will not start until the Java side tells it a card is present.
`LocalServiceHMI` answers that:

```java
void RequestSDCardCheck() {
    if (preSDcardState)
        SetMessgeEvent(0, 1, PATH_SDCARD);      // "card is mounted"
}
```

and `preSDcardState` is set exactly once, in `LocalServiceHMI.onCreate`:

```java
PATH_SDCARD = "mnt/extSdCard";                  // from <init>
if (Environment.getStorageState(new File(PATH_SDCARD)).equals("mounted"))
    preSDcardState = true;
```

`getStorageState` matches the path against the device's mounted volumes.
`/mnt/extSdCard` is not one on any phone, so it returns `unknown`, the flag
stays false, the event is never sent, and the engine waits. Nothing sets it
later either: `sendEventMediaMount` is the only other writer and it only runs
for a `MEDIA_MOUNTED` broadcast whose path equals that same string.

`libn200fs.so` cannot help here — it interposes libc, and
`Environment.getStorageState` goes to the mount service over Binder without
touching a file.

Rewriting the path is not possible for the usual reason: dex `string_ids` are
sorted and `dexopt` checks that at install time, and every path that would
work (`/sdcard`, `/mnt/sdcard`, `/storage/...`) sorts nowhere near `mnt`. So
`tools/patch_sdcard_check.py` replaces the `if-eqz` that guards the assignment
with two `nop`s:

```
 0053  move-result    v10
 0054  if-eqz         v10, +4      ->  nop nop
 0056  iput-boolean   v13, preSDcardState
```

Two code units in, two out. No string moves, the sorted table is untouched,
and the instruction is located by matching the surrounding pattern rather than
a baked-in offset, so a different build is refused instead of corrupted. It is
on by default; `--no-sdcard-check` turns it off.

## Map data on the SD card

`tools/repack.sh --sdcard`. This is the mode to use if the map database is too
big for internal storage, or if it is already on a card.

It ships one extra library, `libn200fs.so` (12 KB, this project's own code,
`shim/n200fs.c`), and makes every app library that opens a file list it as its
**first** `DT_NEEDED`, ahead of `libc.so`. On the pre-N dynamic linker that is
all interposition takes: a library's relocations are resolved against the main
executable, then `LD_PRELOAD`, then its own `DT_NEEDED` list *in order*, and
nothing else. So those libraries call `fopen`, `open`, `stat`, `opendir`,
`mkdir`, `rename`, `system` and the rest here instead of in libc, and each call
maps the hard-coded root onto one that works:

| | |
|---|---|
| read `/mnt/extSdCard///Navkingdom/Common/DB_MAP/x.sti` | → `<card>/Navkingdom/Common/DB_MAP/x.sti` |
| write `/mnt/extSdCard///Navkingdom/LoggerServerPID` | → `/sdcard/n200dt/Navkingdom/LoggerServerPID` |

`/mnt/extSdCard` is not the only root. It is the one in the ELF strings, 288
times, but a device log caught the engine also opening
`/storage/sdcard0/Navkingdom/Persistence/...` and
`/storage/sdcard1/Navkingdom/Common/DB_MAP\map_version.ini` — built at
runtime, so no amount of reading the binaries would have found them. `sdcard1`
is the removable card, which on a Samsung is `/storage/extSdCard`, so every one
of those reads was failing. `\Storage Card` and `\Storage Card2` are in the
binaries verbatim, from this engine's Windows CE days.

All of them are redirected now, and a backslash inside a redirected path is
treated as a separator — on Linux it is an ordinary filename character, so
`DB_MAP\map_version.ini` is one long filename and could never have resolved.
The two `/storage` roots only apply under `Navkingdom/`, so a device where
`/storage/sdcard0` really is primary storage keeps the rest of it.

Three things make this better than rewriting the strings, beyond lifting the
14-byte limit:

* **The split is decided per call, from the open mode** — not from a list of
  paths. A write can therefore not be missed, and a missed write is precisely
  what the tombstone above was.
* **The card is found on the device**, by reading `/proc/mounts` and looking
  for a `Navkingdom` directory under each mounted filesystem. A card is a real
  mount, so whatever the vendor called its mount point — `/storage/extSdCard`,
  `/mnt/extsdcard`, `/storage/sdcard1` — it is in that table, and no list of
  guessed names has to be right. A list of known names is still tried
  afterwards, because a card reached through a *symlink* is not a mount and
  does not appear there.

  Two things make that search harder than it sounds, and both have bitten:
  KitKat mounts primary storage under **several** names at once
  (`/storage/emulated/0`, `/storage/emulated/legacy`, `/mnt/shell/emulated/0`,
  `/storage/sdcard0`) while `EXTERNAL_STORAGE` names only one, so "anything
  that is not primary storage" has to mean all of them; and a `Navkingdom`
  left on internal storage by an earlier install is usually *incomplete* —
  running out of room there is what sends people to a card — so every mount is
  searched for a card-shaped tree before any is searched for the internal
  layout, and a `Navkingdom` holding neither `Navi/` nor `Common/` is not
  accepted at all.
* **Missing write directories are created on demand.** The app assumes its tree
  already exists and never calls `mkdir` for most of it.

Reads look in both roots, and which one is preferred depends on what the file
is:

* **`Navkingdom/Persistence/`** — the app's own mutable state, ~120 `.bak` and
  `.dat` files it writes and reads back. The write root wins, or settings and
  saved routes would reset at every start.
* **everything else** — `Common/`, `Navi/`, `manual/`: read-only payload. The
  **card** wins, even if a stale copy is still sitting on internal storage from
  an earlier install. Preferring the write root here is what makes a
  half-deleted leftover tree shadow the real map data.

The card itself is never written to.

An `fopen` or `open` on one of the app's paths that actually fails is logged by
name, with both the path asked for and the path it was mapped to. The app does
not check the result, so any such failure becomes a SIGSEGV inside `fprintf`
with nothing to say which file was wanted — that is the tombstone this project
started from, and this turns it into three lines in `n200fs.log`. The real
outcome is logged rather than an existence check beforehand, because a file can
be missing, unreadable, on a card that has gone away or refused because its
directory could not be created, and all four end the same way.

```sh
# put the original APK here (it is gitignored)
cp /path/to/n200_signed.apk apk/n200_signed.apk

shim/build.sh                              # needs clang + lld. No NDK.
tools/repack.sh --sdcard --keep-dmb        # -> out/n200-note4.apk
```

Install, and copy the map tree to the card with a file manager or a card reader
— not over `adb push`, which cannot write to secondary storage either:

```sh
adb uninstall com.npnd.n200          # the signature changed; no upgrade in place
adb install out/n200-note4.apk
#  <card>/Navkingdom/Common/DB_MAP/...
#  <card>/Navkingdom/Navi/...
```

Check what it chose:

```sh
adb logcat -c
adb shell am start -n com.npnd.n200/.LocalServiceActivities
adb logcat -d -s n200fs
```

```
I/n200fs  : mounted storage: /storage/emulated/0 /storage/extSdCard
I/n200fs  : read  root: /storage/extSdCard
I/n200fs  : write root: /sdcard/n200dt
I/n200fs  : /mnt/extSdCard -> the two roots above
```

Without a desktop, the same three facts are written to
**`/sdcard/n200dt/n200fs.log`**, readable with any file manager on the phone:

```
n200fs
redirecting /mnt/extSdCard
  reads  -> /storage/extSdCard
  writes -> /storage/emulated/legacy/n200dt

mounted storage:
  /storage/emulated/legacy(internal) /mnt/media_rw/extSdCard /storage/extSdCard

map tree under /storage/extSdCard:
  yes  Navkingdom
  yes  Navkingdom/Navi
  yes  Navkingdom/Navi/CONF
  yes  Navkingdom/Common
  yes  Navkingdom/Common/DB_MAP
  NO   Navkingdom/Persistence
  NO   Navkingdom/manual

Anything the app then failed to open is appended below.
FAILED fopen(read)
    asked for: /mnt/extSdCard///Navkingdom/Navi/DB_HMI/Resources
    looked at: /storage/extSdCard/Navkingdom/Navi/DB_HMI/Resources
```

That answers the three questions in order. No file at all means the library is
not being loaded and the card is not the problem. A wrong `reads` is answered
by `mounted storage`, which lists every storage mount and marks the ones that
are internal. And if the roots are right but the app still dies, the `FAILED`
lines name the file it wanted — the tree report above them usually makes it
obvious which directory never made it onto the card.

If the read root is wrong — a card this build has not heard of, or two cards —
name it yourself. No rebuild, and it takes effect at the next start:

```sh
adb shell 'mkdir -p /sdcard/n200dt &&
           printf "data=/storage/MyCard\nwrite=/sdcard/n200dt\n" \
             > /sdcard/n200dt/n200fs.conf'
```

Writes have to stay on primary storage. KitKat will not let the app write to
the card outside `Android/data/com.npnd.n200/`, so `write=` pointing at the card
would put the `fprintf`-on-NULL crash straight back.

### How the library gets in front of libc

Adding a `DT_NEEDED` entry normally means relinking, which is not an option
here — these are vendor binaries with no source. `tools/inject_needed.py` adds
one in place instead, and it fits because of two coincidences worth knowing
about:

* **`.dynamic` is padded.** Every library in this APK carries four or five
  unused 8-byte slots of zeroes past its `DT_NULL` terminator. One becomes the
  new entry and `DT_NULL` moves up by one. (A modern linker does not do this —
  `lld` packs the section exactly — so the technique works on these libraries
  and would not on a fresh build.)
* **The name can live in that same padding.** `.dynstr` is full in every one of
  these libraries — there is not one dead byte anywhere in it — so the string
  goes into the slots after the new `DT_NULL` and is addressed by its own
  virtual address:

  ```
  d_val = (.dynamic vaddr + 8*(slots_used+1)) - (.dynstr vaddr)
  ```

  Android 4.4 computes `strtab + d_val` and never looks at `DT_STRSZ`, so the
  sum lands on the string. `DT_STRSZ` is widened to cover it anyway: bionic
  grew a bounds check on string indices in Android 7, and it costs nothing to
  satisfy it.

The new entry is made the *first* `DT_NEEDED`, with the existing ones shifted
along so their order relative to each other is untouched. First, not merely
before `libc.so`, because in `libpowertts.so` libc is already first.

Nothing moves: no section header, relocation, symbol or program header changes,
and the repack gate confirms that **no entry changed size relative to the
original**.

Two properties of these libraries had to hold for this to work at all, and
`inject_needed.py --verify` asserts both on every build:

* the redirector must end up ahead of `libc.so`, and
* no library may define one of the interposed names itself — every one of them
  is built `-Bsymbolic`, so the linker searches the library before it looks at
  `DT_NEEDED` at all, and a local definition would win. None do.

## Usage

Requirements: a JDK with `jarsigner`/`keytool`, `zip`/`unzip`, Python 3.9+, and
`readelf`. `--sdcard` needs a clang that can emit ARM, plus `lld`. Only the
optional DMB stub needs an NDK.

**Everything on internal storage** — the original fix, which rewrites the root
to `/sdcard/n200dt` and leaves the vendor libraries alone. Simpler, but the map
database has to fit in internal storage:

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
STUB_TRACE=1 stub/build.sh && tools/repack.sh --sdcard --strip-dead
adb logcat -s n200stub
```

The same trace also goes into `n200fs.log`, beside the map data, prefixed
`stub`. logcat needs a desktop; whoever is installing this is as likely to be
holding the phone, and it puts both halves of startup — which files were
opened, and what the engine asked of DMB/TTS — in one file in order. The stub
finds that hook through `dlsym`, not a `DT_NEEDED`, so a build without the
redirector still links and simply logs nothing there.

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

`.github/workflows/build-shim.yml` does the same for `libn200fs.so`, and needs
no NDK at all — clang is the whole toolchain. It runs three gates in increasing
cost: the injection tests (`tools/test_inject.py`, pure byte surgery, no
compiler), the redirection self-test (`shim/selftest.sh`, the real logic against
the host's libc), then the ARM build. The artifact is named `libn200fs`:

```sh
gh run download --name libn200fs
tools/repack.sh --sdcard --keep-dmb --shim libn200fs.so
```

**Repacking is deliberately not in CI.** This repository is public, and so are
its Actions artifacts and logs. Committing `n200_signed.apk`, fetching it during
a run, or uploading a repacked copy would publish a commercial navigation app
and its map engine to anyone who finds the repo. So CI stops at this project's
own two libraries, and `tools/repack.sh` stays local. Make the repository
private and the repack job becomes safe to add.

## Tools

| | |
|---|---|
| `shim/n200fs.c` | the runtime redirector, with the mapping rules in the header comment |
| `shim/build.sh` | builds and gates it for ARM (exports, SONAME, dependencies). No NDK |
| `shim/selftest.sh` | compiles the same source natively and checks it really redirects, against the host libc; `--sanitize` reruns it under ASan/UBSan |
| `stub/jni/stub_textlayoutshim.c` | 2-symbol replacement for the 5.x-era ABI adapter, with `forc` decoded from the original |
| `tools/inject_needed.py` | adds `libn200fs.so` as the first `DT_NEEDED` of a library, in place, without resizing it; `--verify` is the release gate |
| `tools/test_inject.py` | tests that injection against a hand-built ELF fixture |
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
  headers, relocations and symbol tables stay valid. `classes.dex` is left
  alone — `--skip-dex` on every mode that patches paths. It has to be: the dex
  string table is sorted and `dexopt` verifies that at install time, and
  `/sdcard/n200dt/Navkingdom/Persistence/` sorts past
  `/sdcard/gpsbridge_debug.txt` two slots later, so rewriting it in place makes
  the install fail with no useful error. `tools/patch_paths.py` can still
  rewrite a dex, with the ULEB128 length prefixes, adler32 and SHA-1 all
  maintained, and it refuses rather than shipping a file dexopt would reject;
  no replacement that keeps the ordering has been found.
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
* ~~No repack has been done with the stub.~~ Done, and it found two things.
  `--strip-dead` deleted `libskik.so` on the stated grounds that only
  `libstagefright_dmb.so` needed it — `libtextlayoutshim.so` needs it too, so
  the result could not have loaded `libNV_CNS2K.so`. And the structural gate
  rejected the stub swap outright, because it was written for the path-patch
  flow where no entry may change size; it now takes the list of entries the
  repack replaced on purpose and stays strict about every other one.
  `tools/repack.sh --sdcard --strip-dead` now ends with
  `RESULT: PASS -- nothing outside the NDK-stable ABI remains`, no RISK line
  anywhere in the 25-library load sequence, and 4.4 MB smaller than the
  original.
* Whether the navigation engine tolerates a silent TTS sink. See the tracing
  build above.

### The redirector, specifically

Checked directly:

* The redirection logic is **run, not just inspected**. `shim/selftest.sh`
  compiles `shim/n200fs.c` natively — the same source, nothing stubbed out,
  including its load-time constructor — and exercises it against the host's
  libc in six scenarios: card auto-detected from the built-in mount-point
  list; card found through `/proc/mounts` at a path deliberately *absent* from
  that list, so the mount-table search is what passes it and not the fallback;
  a card competing with an incomplete leftover tree on a second name for
  primary storage, mounted first as a phone would have it; a decoy
  `Navkingdom` holding neither `Navi/` nor `Common/`; a card named by
  `n200fs.conf`; and no card at all. It checks that a map
  read reaches the card, that `fopen(..., "w")` on a path whose directory does
  not exist still returns a stream (the tombstone case) and lands on primary
  storage, that the card is never written to, that written `Persistence` state
  is read back while `Navkingdom/Navi` still comes from the card even with a
  stale copy in the write root, that `system()` has the root rewritten inside
  the command text, and that an unrelated path is passed through untouched.
  `--sanitize` reruns the lot under ASan and UBSan, because the buffer
  handling is hand-rolled against fixed-size stack arrays and a stray byte on
  a phone is a force-close with nothing in logcat.
* The built library is ELF32 / DYN / ARM, SONAME `libn200fs.so`, 12 KB,
  depending on nothing but libc, libdl and liblog, importing four symbols and
  exporting **exactly** the 13 it interposes. An extra export would be bound by
  the app's libraries in place of libc's, so the build fails on one.
* The injection is tested against a hand-built ELF fixture
  (`tools/test_inject.py`): order preserved with the new entry first, the name
  readable by the linker's own rule rather than through section headers,
  `DT_STRSZ` widened, no byte outside `.dynamic` touched, idempotent, and it
  refuses a library with no padding, a name that would not fit, or padding that
  is not zero.
* On the real APK: all 19 libraries that call an interposed entry point take
  the dependency ahead of `libc.so`, none defines one of those names itself,
  the repack gate reports **no entry changed size relative to the original**,
  `classes.dex` is not touched at all, and the signature verifies under both
  `tools/sign_v1.py`'s own check and `jarsigner`.

Not verified:

* **None of it has run on a device.** Everything above is a build-machine
  result. The two things a device decides are whether Android 4.4 accepts a
  `DT_NEEDED` name stored past `DT_STRSZ` — it has no bounds check there, and
  `DT_STRSZ` is widened to cover the string regardless — and whether the
  dynamic linker's mutex tolerates the `dlopen` the constructor makes while the
  linker is loading the library. It is recursive on 4.4, which is why a
  constructor calling `dlopen`/`dlsym` is a common pattern, but that is
  reasoning, not a test. If either is wrong the symptom is unambiguous and
  appears at the first `System.loadLibrary`:

  ```
  dlopen failed: could not load library "..." needed by "libNV_CNS2K.so"
  ```

  and the fallback is the string-rewrite build (`tools/repack.sh --keep-dmb`,
  everything on internal storage), which does not use any of this.
* **Which mount points real devices use.** `/proc/mounts` makes this mostly
  moot — a card is a mount, so it is in the table whatever it is called — but
  a card reachable only through a symlink still depends on the fallback list
  being right. `n200fs.log` and `adb logcat -s n200fs` both print the mount
  table that was found, and `n200fs.conf` overrides the choice without a
  rebuild.
* **The Java half is not redirected.** `java.io.File` goes through libjavacore,
  which this library is not in front of, so the few paths `classes.dex` holds
  still read `/mnt/extSdCard`. None of them block startup:
  `LocalServiceActivities.onCreate` lists
  `/mnt/extSdCard/Navkingdom/Persistence` and, when the directory is simply
  absent, logs `fileList null` and leaves its `bSystemTypeOK` flag at its
  initial `true` — the gate it feeds only fails if that directory *exists* with
  unexpected contents. The rest is `SWUpdater`, screen capture and logging.

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
