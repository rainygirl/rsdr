#ifndef RSDR_APP_H
#define RSDR_APP_H

#include <Application.h>

class MainWindow;

class App : public BApplication {
public:
							App();
	virtual					~App();

	virtual	void			ReadyToRun();

private:
			MainWindow*		fWindow;
};

#endif // RSDR_APP_H
