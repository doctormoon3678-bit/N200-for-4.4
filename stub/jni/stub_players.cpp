// Stub replacement for libstagefright_dmb.so
//
// WHY THIS EXISTS
// ---------------
// The original libstagefright_dmb.so shipped in n200_signed.apk links against
// ~144 *private* platform C++ symbols (android::AudioTrack, android::MediaCodec,
// android::SurfaceComposerClient, android::GraphicBufferMapper,
// android::TextLayoutEngine, android::RefBase/VectorImpl/String8, ...) pulled from
// libstagefright.so, libstagefright_foundation.so, libmedia.so, libgui.so,
// libui.so, libutils.so, libbinder.so and libandroid_runtime.so, plus a private
// Skia copy (libskik.so, SONAME=libskia.so) that drags in libstlport, libft2,
// libicuuc (icu_51), libexpat, libpng and libjpeg.
//
// None of that is a stable ABI. Those symbols were resolved against the
// head-unit's own platform build. On a Samsung Galaxy Note 4 (KitKat) the
// signatures do not match, so the very first dlopen fails and the app dies in
// LocalServiceActivities.<clinit> before any window is shown.
//
// Across all 30 native libraries in the APK, the app itself only ever calls
// 16 symbols out of this library's 1458 exports -- all of them on two classes:
// android::DMBPlayerClass (terrestrial DMB TV tuner/player) and
// android::TTSPlayerClass (PCM playback sink for the Korean TTS engine).
//
// Replacing the library with these 16 no-op symbols removes the entire private
// ABI surface in one step: libskik.so, libstlport.so, libft2.so, libicuuc.so,
// libicui18n.so, libexpat.so, libpng.so, libjpeg.so, libcutils.so, libutils.so,
// libbinder.so, libmedia.so, libgui.so, libui.so, libstagefright.so,
// libstagefright_foundation.so, libandroid_runtime.so and libcorkscrew.so all
// stop being needed. What remains is NDK-stable only.
//
// COST
// ----
//   * DMB TV does not work. Irrelevant on a phone (no DMB tuner hardware).
//   * Voice guidance is silent. NOTE: only the *playback sink* lives here.
//     The Korean TTS synthesis engine (libpowertts.so) needs nothing but
//     libc/libm/liblog and is fully portable, so voice guidance can later be
//     restored by reimplementing TTSPlayerClass on top of OpenSL ES.
//
// ABI NOTES
// ---------
//   * No vtable, typeinfo or destructor of either class is imported by any
//     library in the APK (verified with readelf over all 30 .so files), so these
//     stubs are declared non-polymorphic and with no data members. The ctor
//     bodies write nothing, which is what makes this safe: the *callers* already
//     have sizeof(DMBPlayerClass)/sizeof(TTSPlayerClass) baked in from their own
//     build, and we must not touch storage we do not know the layout of.
//   * C++ return types are not part of Itanium mangling, so the real return
//     types are unknown. Every stub returns 0 in r0, which reads as status_t OK,
//     as false, and as a null/zero handle. See STUB_START_FAILS below if the
//     caller turns out to block waiting on a completion notification.
//   * The notification callback passed to each constructor is deliberately
//     stored but never invoked: the layout of DMB_PLAYER_NOTIFICATION and
//     TTS_PLAYER_NOTIFICATION is unknown, and calling back with a fabricated or
//     null struct would be worse than staying silent. Build with -DSTUB_TRACE=1
//     to see on logcat exactly which stub the engine calls and in which order.
//
// Build:  see ../build.sh  (ndk-build, armeabi-v7a, android-19)
// Verify: the build refuses to install a library whose exported symbol set does
//         not exactly match ../expected_symbols.txt.

#if defined(STUB_TRACE) && STUB_TRACE
// Declared rather than #included: this file is built both with an NDK and
// with a bare clang that has no platform headers, and the prototype is all
// that is needed either way.
extern "C" int __android_log_print(int, const char *, const char *, ...);
#  define TRACE(...) __android_log_print(4 /*INFO*/, "n200stub", __VA_ARGS__)

// logcat needs a desktop. The same trace also goes to n200fs.log, beside the
// map data, where whoever is holding the phone can actually read it -- and
// alongside the file-redirection trace, so one file shows both halves of
// startup. Reached through dlsym, not a DT_NEEDED, so a build with no
// redirector in it still links and simply logs nothing here.
extern "C" void *dlsym(void *, const char *);
#  define RTLD_DEFAULT_32 ((void *)0xffffffff)
static void note(const char *msg)
{
    static void (*fn)(const char *);
    static int tried;
    if (!tried) {
        tried = 1;
        fn = (void (*)(const char *))dlsym(RTLD_DEFAULT_32, "n200fs_note");
    }
    if (fn) fn(msg);
}
#  define NOTE(name) note(name)
#else
#  define TRACE(...) ((void)0)
#  define NOTE(name) ((void)0)
#endif

// Return value handed back for start()/setDataSource()-style entry points.
// 0 == status_t OK == false. If a trace build shows the engine hanging after
// calling start() -- i.e. it is waiting for a "playback finished" notification
// that a silent stub never delivers -- rebuild with -DSTUB_START_FAILS=1 so the
// engine takes its own error path instead.
#if defined(STUB_START_FAILS) && STUB_START_FAILS
#  define STUB_START_RET (-1)
#else
#  define STUB_START_RET (0)
#endif

#define EXPORT __attribute__((visibility("default")))

// Opaque tags, declared at global scope so they mangle exactly as the original
// library's symbols do: P13ANativeWindow, P23DMB_PLAYER_NOTIFICATION,
// P23TTS_PLAYER_NOTIFICATION.
struct ANativeWindow;
struct DMB_PLAYER_NOTIFICATION;
struct TTS_PLAYER_NOTIFICATION;

namespace android {

typedef void (*dmb_notify_t)(void*, DMB_PLAYER_NOTIFICATION*);
typedef void (*tts_notify_t)(void*, TTS_PLAYER_NOTIFICATION*);

// ---------------------------------------------------------------------------
// android::DMBPlayerClass -- terrestrial DMB TV. Called by libdmb_cont.so and
// libNV_GUI.so.
// ---------------------------------------------------------------------------
class EXPORT DMBPlayerClass {
public:
    DMBPlayerClass(dmb_notify_t notify, void* cookie, int sessionId,
                   long flags, bool useHwDecoder);

    int start();
    int stop();
    int reset();
    int setSurface(ANativeWindow* window);
    int setVolume(unsigned int volume);
    int setScanFreq(unsigned int freqKHz);
    int setDataSource(const char* uri, int serviceId, unsigned int subChannel);
};

DMBPlayerClass::DMBPlayerClass(dmb_notify_t notify, void* cookie, int sessionId,
                               long flags, bool useHwDecoder)
{
    (void)notify; (void)cookie; (void)sessionId; (void)flags; (void)useHwDecoder;
    // Intentionally writes nothing: the caller owns this storage and its layout
    // is unknown. See ABI NOTES above.
    NOTE("DMBPlayerClass::ctor()");
    TRACE("DMBPlayerClass::ctor(sessionId=%d, flags=%ld, hw=%d) [stub]",
          sessionId, flags, (int)useHwDecoder);
}

int DMBPlayerClass::start()
{
    NOTE("DMBPlayerClass::start()");
    TRACE("DMBPlayerClass::start() [stub] -> %d", STUB_START_RET);
    return STUB_START_RET;
}

int DMBPlayerClass::stop()
{
    NOTE("DMBPlayerClass::stop()");
    TRACE("DMBPlayerClass::stop() [stub]");
    return 0;
}

int DMBPlayerClass::reset()
{
    NOTE("DMBPlayerClass::reset()");
    TRACE("DMBPlayerClass::reset() [stub]");
    return 0;
}

int DMBPlayerClass::setSurface(ANativeWindow* window)
{
    (void)window;
    NOTE("DMBPlayerClass::setSurface()");
    TRACE("DMBPlayerClass::setSurface(%p) [stub]", (void*)window);
    return 0;
}

int DMBPlayerClass::setVolume(unsigned int volume)
{
    (void)volume;
    NOTE("DMBPlayerClass::setVolume()");
    TRACE("DMBPlayerClass::setVolume(%u) [stub]", volume);
    return 0;
}

int DMBPlayerClass::setScanFreq(unsigned int freqKHz)
{
    (void)freqKHz;
    NOTE("DMBPlayerClass::setScanFreq()");
    TRACE("DMBPlayerClass::setScanFreq(%u) [stub]", freqKHz);
    return 0;
}

int DMBPlayerClass::setDataSource(const char* uri, int serviceId,
                                  unsigned int subChannel)
{
    (void)uri; (void)serviceId; (void)subChannel;
    NOTE("DMBPlayerClass::setDataSource()");
    TRACE("DMBPlayerClass::setDataSource(%s, %d, %u) [stub]",
          uri ? uri : "(null)", serviceId, subChannel);
    return STUB_START_RET;
}

// ---------------------------------------------------------------------------
// android::TTSPlayerClass -- PCM sink for voice guidance. Called by
// libHNcommonlib.so and libGUI_WIDGET.so.
// ---------------------------------------------------------------------------
class EXPORT TTSPlayerClass {
public:
    TTSPlayerClass(tts_notify_t notify, void* cookie, int streamType,
                   int sessionId, char* name);

    int  start(char* pcm, int bytes, int sampleRate);
    int  stop();
    int  pause(bool pause);
    int  reset();
    bool IsStarted();
    int  setVolume(unsigned int volume);
    int  setDataSource(int sampleRate, int channels, unsigned int format);
};

TTSPlayerClass::TTSPlayerClass(tts_notify_t notify, void* cookie, int streamType,
                               int sessionId, char* name)
{
    (void)notify; (void)cookie; (void)streamType; (void)sessionId; (void)name;
    // Intentionally writes nothing. See ABI NOTES above.
    NOTE("TTSPlayerClass::ctor()");
    TRACE("TTSPlayerClass::ctor(stream=%d, session=%d, name=%s) [stub]",
          streamType, sessionId, name ? name : "(null)");
}

int TTSPlayerClass::start(char* pcm, int bytes, int sampleRate)
{
    (void)pcm; (void)bytes; (void)sampleRate;
    // This is where voice guidance audio would be written to an AudioTrack.
    NOTE("TTSPlayerClass::start()");
    TRACE("TTSPlayerClass::start(pcm=%p, bytes=%d, rate=%d) [stub, silent]",
          (void*)pcm, bytes, sampleRate);
    return STUB_START_RET;
}

int TTSPlayerClass::stop()
{
    NOTE("TTSPlayerClass::stop()");
    TRACE("TTSPlayerClass::stop() [stub]");
    return 0;
}

int TTSPlayerClass::pause(bool pause)
{
    (void)pause;
    NOTE("TTSPlayerClass::pause()");
    TRACE("TTSPlayerClass::pause(%d) [stub]", (int)pause);
    return 0;
}

int TTSPlayerClass::reset()
{
    NOTE("TTSPlayerClass::reset()");
    TRACE("TTSPlayerClass::reset() [stub]");
    return 0;
}

bool TTSPlayerClass::IsStarted()
{
    // Nothing is ever playing, so "not started" is the truthful answer.
    NOTE("TTSPlayerClass::IsStarted()");
    TRACE("TTSPlayerClass::IsStarted() [stub] -> false");
    return false;
}

int TTSPlayerClass::setVolume(unsigned int volume)
{
    (void)volume;
    NOTE("TTSPlayerClass::setVolume()");
    TRACE("TTSPlayerClass::setVolume(%u) [stub]", volume);
    return 0;
}

int TTSPlayerClass::setDataSource(int sampleRate, int channels,
                                  unsigned int format)
{
    (void)sampleRate; (void)channels; (void)format;
    NOTE("TTSPlayerClass::setDataSource()");
    TRACE("TTSPlayerClass::setDataSource(%d, %d, %u) [stub]",
          sampleRate, channels, format);
    return STUB_START_RET;
}

} // namespace android
