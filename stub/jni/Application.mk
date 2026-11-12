# The 28 original libraries in lib/armeabi/ are already built as ARMv7
# (Tag_CPU_arch: v7, Tag_FP_arch: VFPv3, Tag_Advanced_SIMD_arch: NEONv1) despite
# sitting in the armeabi directory, and the Galaxy Note 4 is armeabi-v7a, so
# building the stub as armeabi-v7a and installing it into lib/armeabi/ matches
# what is already there.
#
# Set APP_ABI=armeabi here instead if you are on NDK r16b/r17c and want a true
# ARMv5 build; NDK r17+ dropped the armeabi ABI entirely.
APP_ABI      := armeabi-v7a

# minSdkVersion/targetSdkVersion in the APK manifest are both 19.
APP_PLATFORM := android-19

# The stub uses no STL, no exceptions and no RTTI.
APP_STL      := system

APP_OPTIM    := release
