/*
 * Stub replacement for libtextlayoutshim.so.
 *
 * WHAT THE ORIGINAL IS
 * --------------------
 * Not part of the app. Someone else built it, to adapt the head unit's
 * libstagefright_dmb.so to a NEWER Android than the one it was compiled for.
 * It exports the Android 4.4-era platform signatures that libstagefright_dmb
 * imports, and implements them by calling the Android 5.x ones:
 *
 *   exports  android::AudioTrack::AudioTrack(..., transfer_type,
 *                const audio_offload_info_t*, int)              <- 4.4
 *            android::AudioTrack::write(const void*, size_t)     <- 4.4
 *            android::AudioTrack::getPosition(uint32_t*) const   <- 4.4
 *            android::MediaCodec::CreateByType(..., bool)        <- 4.4
 *
 *   imports  android::AudioTrack::AudioTrack(..., transfer_type,
 *                const audio_offload_info_t*, int, int,
 *                const audio_attributes_t*)                      <- 5.x
 *            android::AudioTrack::write(const void*, size_t, bool)
 *            android::AudioTrack::getPosition(uint32_t*)         (non-const)
 *            android::MediaCodec::CreateByType(..., bool, int*)
 *
 * Those four imports are the problem. They are private platform ABI, and they
 * are the 5.x spelling of it, so on a device whose platform is older -- or
 * newer -- than whatever this was built against, loading it fails. It is
 * reached from load #10 of LocalServiceActivities.<clinit>, before a window is
 * drawn, which is early enough that nothing in the app has opened a file yet.
 *
 * WHY IT CAN GO
 * -------------
 * Every one of those 4.4-era exports exists for libstagefright_dmb.so, and
 * nothing else in the APK imports a single one of them. Once
 * libstagefright_dmb.so is itself replaced by stub_players.cpp, the whole
 * adapter is dead weight that still drags libskik.so, libmedia.so and
 * libstagefright.so in behind it.
 *
 * It cannot simply be deleted: libNV_CNS2K.so lists it in DT_NEEDED, so the
 * file has to exist. And libNV_CNS2K.so does import exactly one symbol from
 * it.
 *
 * forc()
 * ------
 * That one symbol. 18 bytes of Thumb-2 in the original:
 *
 *      b480        PUSH   {r7}
 *      af00        ADD    r7, sp, #0
 *      f04f 33ff   MOV.W  r3, #0xffffffff
 *      4618        MOV    r0, r3
 *      46bd        MOV    sp, r7
 *      f85d 7b04   LDR.W  r7, [sp], #4
 *      4770        BX     lr
 *
 * It returns -1 and does nothing else, so it is reproduced here exactly rather
 * than guessed at. What libNV_CNS2K.so makes of the -1 is its own business and
 * is unchanged.
 *
 * noop_vtable_fn() is the same shape returning 0. Nothing outside the original
 * imports it; it is kept so this library is a faithful replacement of the part
 * of the original that anything actually uses.
 */

int forc(void)
{
    return -1;
}

int noop_vtable_fn(void)
{
    return 0;
}
