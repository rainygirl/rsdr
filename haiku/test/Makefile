# Standalone offline tools. No BeAPI, no sound card, no dongle - they run the
# same Demodulator/Dsp code the app uses over a captured I/Q file.
#
# Build on Haiku with the same secondary architecture as the app:
#   setarch x86 make -C test
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++11 -ffast-math -msse2 -mfpmath=sse -Wall -I../src -I../../common

all: demod_file dmb_audio_file live_check usb_rate usb_reset iq_capture iq_burst iq_async

fft_validate: fft_validate.cpp ../../common/Dsp.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ -lm

dmb_audio_file: dmb_audio_file.cpp ../../common/DmbAudio.cpp ../../third_party/bsac/libbsac.a
	$(CXX) $(CXXFLAGS) -I../../third_party/bsac -o $@ \
		dmb_audio_file.cpp ../../common/DmbAudio.cpp ../../third_party/bsac/libbsac.a -lm

../../third_party/bsac/libbsac.a:
	$(MAKE) -C ../../third_party/bsac

# Async capture, the only place in this project that uses read_async: it queues
# many URBs so the device FIFO cannot overflow, and it never cancels - it
# _exit(0)s from the callback, because rtlsdr_cancel_async() crashes Haiku's
# USB stack.
iq_async: iq_async.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ \
		-lrtlsdr -lbe

iq_burst: iq_burst.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ \
		-lrtlsdr -lbe

iq_capture: iq_capture.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ \
		-lrtlsdr -lbe

usb_stress: usb_stress.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ \
		-lrtlsdr -lbe

usb_reset: usb_reset.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ -lusb-1.0

usb_rate: usb_rate.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ \
		-lrtlsdr -lbe

# Offline T-DMB decode, and the regression test for the whole DAB chain: it
# succeeds only if RS(204,188) blocks decode, which nothing but a fully correct
# chain can produce.
msc_file: msc_file.cpp ../../common/DabSync.cpp ../../common/DabFic.cpp \
		../../common/DabMsc.cpp ../../common/DabTables.cpp ../../common/Dsp.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ -lm

# Reproduces the live Receiver's per-6MiB-read processing on a saved capture
# (no inter-read USB loss), to separate the frame-restart effect from the
# hardware gap. mode arg: 0 lose straddler, 1 erasure, 2 pad, 3 per-read reset.
# On a clean capture, mode 0 (carry) ~34%% vs mode 3 (reset) 100%% - which is
# why the Receiver resets DabMsc per read.
msc_chunk: msc_chunk.cpp ../../common/DabSync.cpp ../../common/DabFic.cpp \
		../../common/DabMsc.cpp ../../common/DabTables.cpp ../../common/Dsp.cpp
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ -lm

demod_file: demod_file.cpp ../../common/Demodulator.cpp ../../common/Dsp.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm

# live_check does touch the dongle and the sound card, so it needs libbe
# (BApplication, without which the Media Kit will not create a BSoundPlayer),
# libmedia and librtlsdr - the same set the app links.
live_check: live_check.cpp ../src/Receiver.cpp ../src/SdrDevice.cpp \
		../src/AudioSink.cpp ../../common/Demodulator.cpp ../../common/Dsp.cpp \
		../../common/DabProbe.cpp ../../common/DabFic.cpp ../../common/DabSync.cpp \
		../../common/DabMsc.cpp ../../common/DmbAudio.cpp ../../common/DabTables.cpp \
		../../third_party/bsac/libbsac.a
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ \
		-I../../third_party/bsac -lbe -lmedia -lrtlsdr -lm

dmb_play: dmb_play.cpp ../src/SdrDevice.cpp ../src/AudioSink.cpp ../../common/Dsp.cpp \
		../../common/DabFic.cpp ../../common/DabSync.cpp ../../common/DabMsc.cpp \
		../../common/DmbAudio.cpp ../../common/DabTables.cpp \
		../../third_party/bsac/libbsac.a
	$(CXX) $(CXXFLAGS) -isystem/boot/system/develop/headers/x86 -o $@ $^ \
		-I../../third_party/bsac -lbe -lmedia -lrtlsdr -lm

clean:
	rm -f demod_file dmb_audio_file live_check fft_validate dmb_play msc_chunk

.PHONY: all clean
