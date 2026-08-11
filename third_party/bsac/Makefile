CC ?= gcc
AR ?= ar
CFLAGS ?= -O3 -std=c99 -fPIC -fno-strict-aliasing
CFLAGS += -I. -Ilibavcodec -Ilibavutil -DHAVE_AV_CONFIG_H -D_ISOC99_SOURCE \
	-D_POSIX_C_SOURCE=200112 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE

CODEC_NAMES = aacadtsdec aacdec aacps aacsbr aactab avpacket bitstream bsactab dsputil \
	faanidct fft_float fmtconvert imgconvert jrevdct kbdwin mdct_float \
	mpeg4audio options pthread simple_idct sinewin utils
UTIL_NAMES = audioconvert avstring eval imgutils log mathematics mem opt \
	pixdesc rational samplefmt utils
CODEC_OBJECTS = $(CODEC_NAMES:%=objects/codec/%.o)
UTIL_OBJECTS = $(UTIL_NAMES:%=objects/util/%.o)
OBJECTS = objects/bsac_api.o $(CODEC_OBJECTS) $(UTIL_OBJECTS)

all: libbsac.a

libbsac.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)

objects/bsac_api.o: bsac_api.c
	@mkdir -p objects
	$(CC) $(CFLAGS) -c $< -o $@

objects/codec/%.o: libavcodec/%.c
	@mkdir -p objects/codec
	$(CC) $(CFLAGS) -c $< -o $@

objects/util/%.o: libavutil/%.c
	@mkdir -p objects/util
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf objects libbsac.a

.PHONY: all clean
