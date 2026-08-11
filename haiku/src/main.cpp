#include <Application.h>

#include <csignal>
#include <unistd.h>

#include "App.h"

namespace {

// Turn every signal that would otherwise kill the process into a normal quit
// request, so shutdown closes the dongle properly. Without this, closing the
// terminal that launched the app (SIGHUP) killed it mid-transfer and Haiku's
// libusb died in USBDeviceHandle::TransfersWorker().
void
RequestQuit(int)
{
	if (be_app != NULL)
		be_app_messenger.SendMessage(B_QUIT_REQUESTED);
}

} // namespace

int
main()
{
	App app;

	signal(SIGINT, &RequestQuit);
	signal(SIGTERM, &RequestQuit);
	signal(SIGHUP, &RequestQuit);

	app.Run();

	// _exit(), not return. Everything that needed flushing - the sound card,
	// the capture and demodulator threads, the device handle - was already
	// closed by MainWindow::QuitRequested(). What is skipped is libusb's own
	// atexit teardown, which is where this Haiku build crashes if it has
	// anything left to unwind.
	_exit(0);
}
