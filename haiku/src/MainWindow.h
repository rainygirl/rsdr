#ifndef RSDR_MAIN_WINDOW_H
#define RSDR_MAIN_WINDOW_H

#include <Window.h>

#include "Receiver.h"

class BButton;
class BBitmap;
class BCheckBox;
class BCardLayout;
class BMenuField;
class BSlider;
class BStringView;
class BTextView;
class BMessageRunner;
class BTextControl;
class SpectrumView;
class WaterfallView;
class DmbVideoView;
class SignalGauge;

class MainWindow : public BWindow {
public:
							MainWindow();
	virtual					~MainWindow();

	virtual	void			MessageReceived(BMessage* message);
	virtual	bool			QuitRequested();

private:
			void			_BuildLayout();
			void			_ApplyFrequencyFromField();
			void			_SetFrequency(uint64 hz);
			void			_UpdateFrequencyField();
			void			_SelectMode(demod_mode mode,
								bool beginDmbProbe = true);
			void			_ToggleRunning();
			void			_SetTransportRunning(bool running);
			void			_UpdateControlsForMode();
			void			_UpdateTransportVisibility();
			void			_ResetDmbStations();
			void			_BeginDmbProbe();
			void			_Tick();
	static	int32			_StopDmbProbeEntry(void* cookie);

			Receiver		fReceiver;

			BTextControl*	fFrequencyField;
			BMenuField*		fModeField;
			BMenuField*		fPresetField;
			BCheckBox*		fStereoBox;
			BSlider*		fSquelchSlider;
			BSlider*		fVolumeSlider;
			BButton*		fStartButton;
			BBitmap*		fPlayIcon;
			BBitmap*		fStopIcon;
			BTextView*		fStatusView;
			BStringView*	fMeterView;
	// DMB only: the ensemble and its service list, decoded from the FIC.
			BStringView*	fEnsembleView;
			BMenuField*		fServicesView;
			SpectrumView*	fSpectrum;
			WaterfallView*	fWaterfall;
			DmbVideoView*	fDmbVideo;
			SignalGauge*	fSignalGauge;
			BCardLayout*	fDisplayLayout;
	// The card holding the waterfall/video. Hidden in T-DMB: Haiku decodes
	// audio only (no H.264), so the video card was a permanent black
	// "Waiting for T-DMB signal..." box with nothing to show.
			BView*			fDisplay;
	// BWindow::Pulse() only fires if some view asks for it with
	// B_PULSE_NEEDED; none of these do, and the displays simply never
	// updated. An explicit runner is unambiguous.
			BMessageRunner*	fTicker;

			uint64			fFrequency;
			demod_mode		fMode;
			uint32			fLastSpectrumSequence;
			int				fSelectedDmbSubchannel;
			bool			fDmbProbing;
			bool			fDmbProbeStopping;
			bool			fDmbStandby;
};

#endif // RSDR_MAIN_WINDOW_H
