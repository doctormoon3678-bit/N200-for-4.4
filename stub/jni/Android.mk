LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# Must be exactly this: the output filename and SONAME have to stay
# libstagefright_dmb.so, because libdmb_cont.so, libHNcommonlib.so,
# libGUI_WIDGET.so and libNV_GUI.so all carry it as a DT_NEEDED entry.
LOCAL_MODULE    := stagefright_dmb
LOCAL_SRC_FILES := stub_players.cpp

# -fvisibility=default: the 16 symbols must land in .dynsym.
# No -ffunction-sections/--gc-sections and no LTO, so nothing gets dropped.
LOCAL_CPPFLAGS  := -O2 -fno-exceptions -fno-rtti -fvisibility=default -fno-lto -Wall

# liblog only (NDK-stable). Used by the optional -DSTUB_TRACE=1 build.
LOCAL_LDLIBS    := -llog

# Trace build: STUB_TRACE=1 ./build.sh
ifeq ($(STUB_TRACE),1)
  LOCAL_CPPFLAGS += -DSTUB_TRACE=1
endif
# Make start()/setDataSource() report failure instead of success:
#   STUB_START_FAILS=1 ./build.sh
ifeq ($(STUB_START_FAILS),1)
  LOCAL_CPPFLAGS += -DSTUB_START_FAILS=1
endif

include $(BUILD_SHARED_LIBRARY)
