/*
 * USB-level reset for a wedged RTL-SDR.
 *
 * Failure mode this exists for: after certain read patterns the dongle's
 * bulk IN endpoint stops delivering while control transfers keep working -
 * rtl_test still finds the tuner and reads its registers, but every
 * rtlsdr_read_sync() returns -1 and rtl_sdr exits with "Library error -1".
 * librtlsdr has no way out of that state; only a bus-level reset (or a
 * replug) clears it.
 */
#include <cstdio>

#include <libusb-1.0/libusb.h>

int
main()
{
	libusb_context* ctx = NULL;
	if (libusb_init(&ctx) < 0) {
		fprintf(stderr, "libusb_init failed\n");
		return 1;
	}

	libusb_device** list = NULL;
	ssize_t count = libusb_get_device_list(ctx, &list);
	int reset = 0;

	for (ssize_t i = 0; i < count; i++) {
		libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(list[i], &desc) < 0)
			continue;
		// Realtek RTL2832U, in all the vendor/product combinations the
		// generic dongles ship with.
		if (desc.idVendor != 0x0bda)
			continue;
		if (desc.idProduct != 0x2832 && desc.idProduct != 0x2838)
			continue;

		libusb_device_handle* handle = NULL;
		int err = libusb_open(list[i], &handle);
		if (err < 0) {
			fprintf(stderr, "libusb_open failed: %s\n",
				libusb_error_name(err));
			continue;
		}
		printf("found %04x:%04x, resetting... ", desc.idVendor,
			desc.idProduct);
		err = libusb_reset_device(handle);
		printf("%s\n", err == 0 ? "ok" : libusb_error_name(err));
		libusb_close(handle);
		reset++;
	}

	libusb_free_device_list(list, 1);
	libusb_exit(ctx);

	if (reset == 0) {
		fprintf(stderr, "no RTL2832 device found\n");
		return 1;
	}
	return 0;
}
