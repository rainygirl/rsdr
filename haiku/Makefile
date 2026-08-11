## Haiku Generic Build Makefile ##
NAME = rsdr
TYPE = APP
APP_MIME_SIG = application/x-vnd.RSDR-SDR

SRCS = \
	src/main.cpp \
	src/App.cpp \
	src/MainWindow.cpp \
	src/DmbVideoView.cpp \
	src/SignalGauge.cpp \
	src/Localize.cpp \
	src/SpectrumView.cpp \
	src/WaterfallView.cpp \
	../common/Dsp.cpp \
	../common/DabProbe.cpp \
	../common/DabFic.cpp \
	../common/DabTables.cpp \
	../common/DabSync.cpp \
	../common/DabMsc.cpp \
	../common/DmbAudio.cpp \
	src/SdrDevice.cpp \
	../common/Demodulator.cpp \
	src/AudioSink.cpp \
	src/Receiver.cpp \
	../common/Bands.cpp

# The DSP and DAB decoders live in ../common because the macOS target
# builds the very same files. Only the BeAPI layers are in src/.
RDEFS = src/app.rdef
RSRCS =

# THIS APP MUST BE BUILT FOR THE "x86" SECONDARY ARCHITECTURE:
#
#     setarch x86 make
#
# librtlsdr is only packaged for that arch on an x86_gcc2 system
# (rtl_sdr_x86 / rtl_sdr_x86_devel from HaikuDepot - the package that also
# provides rtl_test/rtl_fm/rtl_power in /boot/system/bin). Its .so lives in
# /boot/system/develop/lib/x86 and its headers in
# /boot/system/develop/headers/x86, neither of which the primary gcc2
# toolchain looks at - and gcc 2.95 could not compile this code anyway.
# install.sh calls setarch for you.
#
# rtlsdr: rtlsdr_open/set_center_freq/read_sync - see SdrDevice.cpp
# media:  BSoundPlayer (Media Kit) - see AudioSink.cpp
# be:     BApplication/BWindow/interface kit
# stdc++/supc++: makefile-engine does not link these for TYPE=APP, and
#         std::vector reallocation needs out-of-line symbols from them.
LIBS = be media rtlsdr avcodec avutil swscale stdc++ supc++ ../third_party/bsac/libbsac.a

LIBPATHS =
SYSTEM_INCLUDE_PATHS = /boot/system/develop/headers/x86
LOCAL_INCLUDE_PATHS = src ../common
OPTIMIZE := FULL
LOCALES =
DEFINES =
WARNINGS = ALL
SYMBOLS =
DEBUGGER =
# -ffast-math: the DSP inner loops are plain float dot products where strict
# IEEE semantics buy nothing. -msse2 is safe on every CPU this can run on
# (the Atom Z520 this was developed against has SSE2/SSSE3) and lets gcc
# vectorize the FIR loops, which is the difference between the wideband FM
# chain costing ~20% and ~50% of that CPU.
# Keep the bundled, old Libav BSAC headers behind the system FFmpeg headers.
# Putting both at the same precedence lets FFmpeg 6 accidentally include an
# ancient libavutil header with the same name from third_party/bsac.
COMPILER_FLAGS = -std=c++11 -ffast-math -msse2 -mfpmath=sse \
	-ftree-vectorize -fno-math-errno -fno-trapping-math \
	-idirafter../third_party/bsac
LINKER_FLAGS =
APP_VERSION =
DRIVER_PATH =

## Include the Makefile-Engine that ships with the Haiku SDK.
include /boot/system/develop/etc/makefile-engine

../third_party/bsac/libbsac.a:
	$(MAKE) -C ../third_party/bsac

$(TARGET): ../third_party/bsac/libbsac.a
