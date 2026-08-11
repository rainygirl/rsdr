# macOS build.
#
# The DSP and DAB sources live in ../common and are built by both targets from
# that one place. macos/src goes first on the include path so its SupportDefs.h
# and OS.h shims stand in for Haiku's, which is all those files need from the
# BeAPI - they are otherwise plain C++.
#
# What is *not* shared is everything that touches the operating system. The
# Haiku app's SdrDevice, AudioSink, Receiver and windows are BeAPI; the macOS
# target has its own device and CoreAudio layers under macos/src.
#
#   make            offline tools (no dongle, no audio hardware needed)
#   make rsdr-cli   live receiver - needs `brew install librtlsdr`
#
COMMON = ../common
CXX ?= clang++
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wno-unused-function -Isrc -I$(COMMON) \
	-I../third_party/bsac

# The shared core keeps its portable FFT for Haiku.  macOS selects a
# thread-local Accelerate/vDSP backend without changing the DSP API.
DSP_ACCEL_CFLAGS = -DRSDR_USE_ACCELERATE=1
DSP_ACCEL_LIBS = -framework Accelerate

BREW := $(shell brew --prefix 2>/dev/null)
RTL_CFLAGS := -I$(BREW)/include
RTL_LIBS := -L$(BREW)/lib -lrtlsdr
IOKIT := -framework IOKit -framework CoreFoundation

# Portable core, byte for byte the same files the Haiku app builds.
CORE = $(COMMON)/Dsp.cpp \
	$(COMMON)/Demodulator.cpp \
	$(COMMON)/DabSync.cpp \
	$(COMMON)/DabFic.cpp \
	$(COMMON)/DabMsc.cpp \
	$(COMMON)/DabTables.cpp

APP_SRCS = src/main_app.mm src/VideoView.mm src/CoreAudioSink.cpp \
	src/RtlDevice.cpp $(CORE) $(COMMON)/Bands.cpp $(COMMON)/AirBands.cpp \
	$(COMMON)/AirMonitor.cpp $(COMMON)/DmbAudio.cpp \
	$(COMMON)/TsDescrambler.cpp \
	../third_party/bsac/libbsac.a

# GNU make does not treat quotes as escaping in a target name.  The old
# `"R SDR.icns"` rule was therefore parsed as two targets and ran make_icon
# twice under -j, leaving a half-written iconset that iconutil rejected.
ICON = R\ SDR.icns

all: msc_file demod_file air_probe ts_probe ca_probe ts_descramble dab_live app

# Offline T-DMB decode, and the regression test for the DAB chain. Identical
# source to the Haiku one, so a capture decoded here and there must agree -
# verified: both report RS 70 of 70 blocks on the same capture.
msc_file: ../haiku/test/msc_file.cpp $(CORE)
	$(CXX) $(CXXFLAGS) $(DSP_ACCEL_CFLAGS) -o $@ $^ -lm $(DSP_ACCEL_LIBS)

demod_file: ../haiku/test/demod_file.cpp $(COMMON)/Demodulator.cpp \
		$(COMMON)/Dsp.cpp
	$(CXX) $(CXXFLAGS) $(DSP_ACCEL_CFLAGS) -o $@ $^ -lm $(DSP_ACCEL_LIBS)

# The app's live T-DMB path without the app, for when the window shows zero
# frames but an offline capture from the same dongle decodes perfectly.
dab_live: src/dab_live.cpp src/RtlDevice.cpp $(COMMON)/DabSync.cpp \
		$(COMMON)/DabFic.cpp $(COMMON)/DabTables.cpp $(COMMON)/Dsp.cpp
	$(CXX) $(CXXFLAGS) $(DSP_ACCEL_CFLAGS) $(RTL_CFLAGS) -o $@ $^ \
		$(RTL_LIBS) $(IOKIT) -lm $(DSP_ACCEL_LIBS)

# Checks a recovered transport stream offline: BSAC audio and H.264 video.
ts_probe: src/ts_probe.cpp $(COMMON)/DmbAudio.cpp ../third_party/bsac/libbsac.a
	$(CXX) $(CXXFLAGS) -I../third_party/bsac -o $@ $^ -lm

# Parses CAT/PMT CA descriptors and inventories/dumps private ECM/EMM
# sections without requiring a key or making assumptions about the CAS.
ca_probe: src/ca_probe.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

# Uses libdvbcsa through dlopen, so the GPL library remains an optional,
# separately installed component rather than being copied into the app.
ts_descramble: src/ts_descramble.cpp $(COMMON)/TsDescrambler.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ -ldl

# Exercises the gain+frequency pair emitted by every preset while async USB
# capture is active. This caught the duplicate-cancel/libusb control race.
tune_stress: src/tune_stress.cpp src/RtlDevice.cpp
	$(CXX) $(CXXFLAGS) $(RTL_CFLAGS) -o $@ $^ $(RTL_LIBS) $(IOKIT)

# Measures how far each airband channel stands above the noise floor, so the
# squelch threshold can be set from data instead of intuition.
air_probe: src/air_probe.cpp $(COMMON)/AirBands.cpp $(COMMON)/AirMonitor.cpp \
		$(COMMON)/Dsp.cpp
	$(CXX) $(CXXFLAGS) $(DSP_ACCEL_CFLAGS) $(RTL_CFLAGS) -o $@ $^ \
		$(RTL_LIBS) -lm $(DSP_ACCEL_LIBS)

# The bundle. Make cannot have a target with a space in it, so this is a phony
# rule that produces "R SDR.app". librtlsdr and libusb are copied inside and
# their load paths rewritten, so the app runs on a Mac without Homebrew.
# The icon is rendered from the same isometric geometry and palette as the
# Haiku one - see src/make_icon.mm. HVIF cannot be converted, so the shape is
# rebuilt rather than translated.
make_icon: src/make_icon.mm
	$(CXX) -O2 -std=c++11 -o $@ $^ -framework Foundation \
		-framework CoreGraphics -framework ImageIO \
		-framework UniformTypeIdentifiers

$(ICON): make_icon
	./make_icon "R SDR.iconset"
	iconutil -c icns "R SDR.iconset" -o "R SDR.icns"

app: $(APP_SRCS) package.sh $(ICON)
	rm -rf "R SDR.app"
	mkdir -p "R SDR.app/Contents/MacOS" "R SDR.app/Contents/Frameworks"
	$(CXX) $(CXXFLAGS) $(DSP_ACCEL_CFLAGS) $(RTL_CFLAGS) \
		-o "R SDR.app/Contents/MacOS/R SDR" \
		$(APP_SRCS) $(RTL_LIBS) -lm \
		-ldl \
		$(IOKIT) \
		-framework Cocoa -framework AudioToolbox -framework CoreFoundation \
		-framework AVFoundation -framework CoreMedia $(DSP_ACCEL_LIBS)
	mkdir -p "R SDR.app/Contents/Resources"
	cp "R SDR.icns" "R SDR.app/Contents/Resources/"
	sh package.sh
	@echo "built R SDR.app"

install: app
	rm -rf "/Applications/R SDR.app"
	cp -R "R SDR.app" /Applications/
	@echo "installed /Applications/R SDR.app"

clean:
	rm -rf msc_file demod_file rsdr-cli air_probe ts_probe ca_probe ts_descramble \
		dab_live tune_stress make_icon \
		"R SDR.app" "R SDR.iconset" "R SDR.icns"

.PHONY: all app install clean
