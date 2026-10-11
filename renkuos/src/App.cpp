#include "App.h"

#include "MainWindow.h"

App::App()
	:
	BApplication("application/x-vnd.RSDR-SDR"),
	fWindow(NULL)
{
}

App::~App()
{
}

void
App::ReadyToRun()
{
	fWindow = new MainWindow();
	fWindow->Show();
}
