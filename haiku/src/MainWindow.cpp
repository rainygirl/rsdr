#include "MainWindow.h"

#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <CardLayout.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <Slider.h>
#include <StringView.h>
#include <TextView.h>
#include <TextControl.h>
#include <View.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Bands.h"
#include "Localize.h"
#include "DmbVideoView.h"
#include "SignalGauge.h"
#include "SpectrumView.h"
#include "WaterfallView.h"

namespace {

const uint32 kMsgFrequency	= 'freQ';
const uint32 kMsgMode		= 'modE';
const uint32 kMsgPreset		= 'preS';
const uint32 kMsgSquelch	= 'sqeL';
const uint32 kMsgStereo		= 'steR';
const uint32 kMsgVolume		= 'voLu';
const uint32 kMsgStartStop	= 'strT';
const uint32 kMsgDmbService = 'dmbS';
const uint32 kMsgTick		= 'ticK';
const uint32 kMsgDmbProbeStopped = 'dpSt';

BBitmap*
MakeTransportIcon(bool running)
{
	const int size = 18;
	BBitmap* icon = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
	if (icon->InitCheck() != B_OK || icon->Bits() == NULL) {
		delete icon;
		return NULL;
	}
	memset(icon->Bits(), 0, icon->BitsLength());
	uint8* bits = (uint8*)icon->Bits();
	int32 bpr = icon->BytesPerRow();
	for (int y = 3; y <= 14; y++) {
		int left;
		int right;
		if (running) {
			left = 4;
			right = 14;
		} else {
			int distance = y < 9 ? y - 3 : 14 - y;
			left = 5;
			right = 5 + distance * 2;
		}
		for (int x = left; x <= right; x++) {
			uint8* pixel = bits + y * bpr + x * 4;
			pixel[0] = 35;
			pixel[1] = 35;
			pixel[2] = 35;
			pixel[3] = 255;
		}
	}
	return icon;
}

} // namespace

MainWindow::MainWindow()
	:
	BWindow(BRect(60, 60, 940, 640), "R SDR", B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE),
	fFrequencyField(NULL),
	fModeField(NULL),
	fPresetField(NULL),
	fStereoBox(NULL),
	fSquelchSlider(NULL),
	fVolumeSlider(NULL),
	fStartButton(NULL),
	fPlayIcon(NULL),
	fStopIcon(NULL),
	fStatusView(NULL),
	fMeterView(NULL),
	fEnsembleView(NULL),
	fServicesView(NULL),
	fSpectrum(NULL),
	fWaterfall(NULL),
	fDmbVideo(NULL),
	fSignalGauge(NULL),
	fDisplayLayout(NULL),
	fDisplay(NULL),
	fTicker(NULL),
	fFrequency(92500000ULL),
	fMode(kModeWFM),
	fLastSpectrumSequence(0),
	fSelectedDmbSubchannel(-1),
	fDmbProbing(false),
	fDmbProbeStopping(false),
	fDmbStandby(false)
{
	TrInit();
	_BuildLayout();

	fReceiver.SetFrequency(fFrequency);
	fReceiver.SetMode(fMode);
	fReceiver.SetVolume(0.7f);
	fReceiver.SetSquelchDb(-200.0f);
	fReceiver.SetGain(Receiver::kDefaultGainTenths);

	_UpdateFrequencyField();
	_UpdateControlsForMode();

	// 30 Hz keeps the analogue waveform and waterfall smooth.  The receiver
	// publishes spectrum data at a lower cadence, so the UI can present each
	// newly published trace promptly instead of bunching updates into motion. DMB's
	// expensive decode path is handled on the receiver thread and does not
	// require a faster UI cadence.
	//
	// A BMessageRunner rather than SetPulseRate(): BWindow only generates
	// pulses when some view has asked for them with B_PULSE_NEEDED, so with
	// SetPulseRate alone Pulse() was never called and the spectrum and
	// waveform stayed empty forever.
	fTicker = new BMessageRunner(BMessenger(this), new BMessage(kMsgTick),
		33333);
}

MainWindow::~MainWindow()
{
	delete fTicker;
	fReceiver.Stop();
	delete fPlayIcon;
	delete fStopIcon;
}

void
MainWindow::_BuildLayout()
{
	fFrequencyField = new BTextControl("freq", "MHz:", "92.500",
		new BMessage(kMsgFrequency));
	fFrequencyField->SetModificationMessage(NULL);
	fFrequencyField->SetExplicitMinSize(BSize(180, B_SIZE_UNSET));
	fFrequencyField->SetExplicitMaxSize(BSize(230, B_SIZE_UNSET));

	BPopUpMenu* modeMenu = new BPopUpMenu("mode");
	for (int i = 0; i < kModeCount; i++) {
		BMessage* msg = new BMessage(kMsgMode);
		msg->AddInt32("mode", i);
		BMenuItem* item = new BMenuItem(ModeName((demod_mode)i), msg);
		modeMenu->AddItem(item);
		if (i == (int)fMode)
			item->SetMarked(true);
	}
	fModeField = new BMenuField("modefield", NULL, modeMenu);
	fModeField->SetExplicitMinSize(BSize(86, B_SIZE_UNSET));

	BPopUpMenu* presetMenu = new BPopUpMenu(Tr("Presets"));
	for (int i = 0; i < PresetCount(); i++) {
		BMessage* msg = new BMessage(kMsgPreset);
		msg->AddInt32("preset", i);
		presetMenu->AddItem(new BMenuItem(PresetAt(i).label, msg));
	}
	fPresetField = new BMenuField("presetfield", NULL, presetMenu);
	fPresetField->SetExplicitMinSize(BSize(210, B_SIZE_UNSET));
	fPresetField->SetExplicitMaxSize(BSize(250, B_SIZE_UNSET));

	fSquelchSlider = new BSlider("squelch", Tr("Squelch"),
		new BMessage(kMsgSquelch), -100, 0, B_HORIZONTAL);
	fSquelchSlider->SetValue(-100);

	fStereoBox = new BCheckBox("stereo", Tr("Stereo"), new BMessage(kMsgStereo));
	fStereoBox->SetValue(B_CONTROL_OFF);

	fVolumeSlider = new BSlider("volume", Tr("Volume"),
		new BMessage(kMsgVolume), 0, 100, B_HORIZONTAL);
	fVolumeSlider->SetValue(70);

	// Same problem as the status views: these labels change width as the value
	// changes ("Squelch: off" versus "Squelch: -50 dBFS").
	fSquelchSlider->SetExplicitMinSize(BSize(150, B_SIZE_UNSET));
	fVolumeSlider->SetExplicitMinSize(BSize(150, B_SIZE_UNSET));

	fPlayIcon = MakeTransportIcon(false);
	fStopIcon = MakeTransportIcon(true);
	fStartButton = new BButton("start", "", new BMessage(kMsgStartStop));
	fStartButton->SetExplicitMinSize(BSize(38, 30));
	fStartButton->SetExplicitMaxSize(BSize(38, 30));
	_SetTransportRunning(false);

	fStatusView = new BTextView("status");
	fStatusView->SetText(Tr("idle"));
	fStatusView->MakeEditable(false);
	fStatusView->SetWordWrap(true);
	fStatusView->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
	fStatusView->SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
	fMeterView = new BStringView("meter", "");
	// A BStringView's minimum width is the width of its text, and with
	// B_AUTO_UPDATE_SIZE_LIMITS the window grows to satisfy it - so the window
	// visibly jumped wider and narrower whenever live diagnostics changed.
	// Pinning the minimum lets the text vary without moving the window.
	fStatusView->SetExplicitMinSize(BSize(80, 32));
	fStatusView->SetExplicitMaxSize(BSize(B_SIZE_UNSET, 42));
	fMeterView->SetExplicitMinSize(BSize(80, B_SIZE_UNSET));

	fEnsembleView = new BStringView("ensemble", "");
	BPopUpMenu* serviceMenu = new BPopUpMenu("T-DMB services");
	BMenuItem* stationPlaceholder = new BMenuItem("Station...", NULL);
	stationPlaceholder->SetEnabled(false);
	stationPlaceholder->SetMarked(true);
	serviceMenu->AddItem(stationPlaceholder);
	fServicesView = new BMenuField("services", NULL, serviceMenu);
	fEnsembleView->SetExplicitMinSize(BSize(80, B_SIZE_UNSET));
	fServicesView->SetExplicitMinSize(BSize(130, B_SIZE_UNSET));
	fServicesView->SetExplicitMaxSize(BSize(170, B_SIZE_UNSET));
	fServicesView->SetEnabled(false);

	fSpectrum = new SpectrumView("spectrum");
	fWaterfall = new WaterfallView("waterfall");
	fDmbVideo = new DmbVideoView("DMB video");
	fSignalGauge = new SignalGauge("signal gauge");
	BView* analogDisplay = new BView("analog display", B_WILL_DRAW);
	BLayoutBuilder::Group<>(analogDisplay, B_VERTICAL, 1)
		// Haiku's Atom is CPU-bound during reception; keep the waterfall as
		// the sole analogue display.  The spectrum object remains headless so
		// it can maintain the dB range used to colour the waterfall.
		.Add(fWaterfall, 1.0f);
	fDisplay = new BView("receiver display", B_WILL_DRAW);
	fDisplayLayout = new BCardLayout();
	fDisplay->SetLayout(fDisplayLayout);
	fDisplayLayout->AddView(analogDisplay);
	fDisplayLayout->AddView(fDmbVideo);
	fDisplayLayout->SetVisibleItem((int32)0);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING,
				B_USE_WINDOW_SPACING, B_USE_SMALL_SPACING)
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
				.Add(fPresetField)
				.Add(fModeField)
				.Add(fFrequencyField)
				.Add(fServicesView)
				.Add(fStartButton)
			.End()
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
				.Add(fVolumeSlider)
				.Add(fSquelchSlider)
				.Add(fStereoBox)
				.Add(fMeterView)
			.End()
		.End()
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING, 0, B_USE_WINDOW_SPACING, 0)
			.Add(fSignalGauge, 0.0f)
			.Add(fDisplay, 1.0f)
		.End()
		.AddGroup(B_VERTICAL, 0)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_SMALL_SPACING,
				B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING)
			.Add(fStatusView)
			.Add(fEnsembleView)
		.End()
	.End();

	// These controls are DMB-only. Set the initial hidden state after the
	// layout has attached its views; testing IsHidden() before attachment can
	// report the attachment state rather than an explicit Hide(), leaving the
	// disabled Station field occupying the bottom of the FM window.
	fEnsembleView->Hide();
	fServicesView->Hide();

	fFrequencyField->SetTarget(this);
}

bool
MainWindow::QuitRequested()
{
	fReceiver.Stop();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}

void
MainWindow::_SetFrequency(uint64 hz)
{
	// Direct sampling covers LF/HF; the R820T2 path takes over above it.
	const uint64 kMinHz = 100000ULL;
	const uint64 kMaxHz = 1766000000ULL;
	if (hz < kMinHz)
		hz = kMinHz;
	if (hz > kMaxHz)
		hz = kMaxHz;

	fFrequency = hz;
	fReceiver.SetFrequency(hz);
	_UpdateFrequencyField();
}

void
MainWindow::_UpdateFrequencyField()
{
	char buf[32];
	if (fMode == kModeAM) {
		fFrequencyField->SetLabel("kHz:");
		snprintf(buf, sizeof(buf), "%.1f", (double)fFrequency / 1e3);
	} else {
		fFrequencyField->SetLabel("MHz:");
		snprintf(buf, sizeof(buf), "%.4f", (double)fFrequency / 1e6);
	}
	fFrequencyField->SetText(buf);
}

void
MainWindow::_ApplyFrequencyFromField()
{
	double value = atof(fFrequencyField->Text());
	if (value <= 0.0) {
		_UpdateFrequencyField();
		return;
	}
	double scale = fMode == kModeAM ? 1e3 : 1e6;
	_SetFrequency((uint64)(value * scale + 0.5));
}

void
MainWindow::_SelectMode(demod_mode mode, bool beginDmbProbe)
{
	bool wasAir = fMode == kModeAir;
	bool enteringDmb = mode == kModeDMB && fMode != kModeDMB;
	bool leavingDmb = mode != kModeDMB && fMode == kModeDMB;
	fMode = mode;
	fReceiver.SetMode(mode);
	// The connected Haiku dongle needs -84 ppm for analogue FM calibration,
	// but real 8B DAB measurements are the opposite: -84 gives OFDM lock with
	// FIB 0/72, while 0 ppm gives FIB 12/12 and a complete station list on the
	// first analysis.  Keep the independently measured correction per path.
	if (enteringDmb)
		fReceiver.SetPpm(0);
	else if (leavingDmb)
		fReceiver.SetPpm(-84);
	if (mode != kModeDMB)
		fDmbProbing = false;
	if (mode != kModeDMB)
		fDmbStandby = false;
	if (enteringDmb)
		_ResetDmbStations();
	// T-DMB gain is selected from the tuner's actual supported steps by the
	// Receiver's FIB-CRC sweep.  It is deliberately not tied to an ensemble,
	// frequency, or station.  Restore the analogue default on the way out.
	if (leavingDmb)
		fReceiver.SetGain(Receiver::kDefaultGainTenths);
	if (mode == kModeAir && !wasAir) {
		// Single-frequency Air reception starts on the published RKSS/Seoul
		// Approach frequency. Wide city/band scanning was deliberately removed:
		// repeated retunes created regular audio gaps and false detections.
		_SetFrequency(119100000ULL);
		fReceiver.SetScanning(false);
		fSquelchSlider->SetValue(-100);
		fReceiver.SetSquelchDb(-200.0f);
	} else if (wasAir && mode != kModeAir)
		fReceiver.SetScanning(false);
	_UpdateFrequencyField();

	BMenu* menu = fModeField->Menu();
	if (menu != NULL) {
		for (int32 i = 0; i < menu->CountItems(); i++) {
			BMenuItem* item = menu->ItemAt(i);
			if (item != NULL)
				item->SetMarked(i == (int32)mode);
		}
	}
	_UpdateControlsForMode();
	_UpdateTransportVisibility();
	if (mode == kModeDMB && beginDmbProbe)
		_BeginDmbProbe();
}

void
MainWindow::_UpdateControlsForMode()
{
	// Squelch only means something for the modes where a closed squelch is
	// silence rather than a missing carrier.
	bool squelchUseful = fMode == kModeAM
		|| fMode == kModeAir
		|| fMode == kModeLSB || fMode == kModeUSB;
	fSquelchSlider->SetEnabled(squelchUseful);
	if (fMode == kModeDMB) {
		// Haiku decodes T-DMB audio only (no H.264), so the video card was a
		// permanent black "Waiting for T-DMB signal..." box. Hide the whole
		// display area; the ensemble line and Station menu carry the state.
		fDmbVideo->Reset();
		if (fDisplay != NULL && !fDisplay->IsHidden())
			fDisplay->Hide();
		if (fSignalGauge != NULL && fSignalGauge->IsHidden())
			fSignalGauge->Show();
		if (fEnsembleView->IsHidden())
			fEnsembleView->Show();
		if (fServicesView->IsHidden())
			fServicesView->Show();
	} else {
		fDisplayLayout->SetVisibleItem((int32)0);
		fDmbVideo->Reset();
		if (fDisplay != NULL && fDisplay->IsHidden())
			fDisplay->Show();
		if (fSignalGauge != NULL && !fSignalGauge->IsHidden())
			fSignalGauge->Hide();
		if (!fEnsembleView->IsHidden())
			fEnsembleView->Hide();
		if (!fServicesView->IsHidden())
			fServicesView->Hide();
	}
	if (fMode == kModeWFM) {
		if (fStereoBox->IsHidden())
			fStereoBox->Show();
	} else if (!fStereoBox->IsHidden())
		fStereoBox->Hide();
}

void
MainWindow::_UpdateTransportVisibility()
{
	bool hide = fMode == kModeDMB && fSelectedDmbSubchannel < 0;
	if (hide && !fStartButton->IsHidden())
		fStartButton->Hide();
	else if (!hide && fStartButton->IsHidden())
		fStartButton->Show();
}

void
MainWindow::_SetTransportRunning(bool running)
{
	if (fStartButton == NULL)
		return;
	fStartButton->SetLabel("");
	fStartButton->SetIcon(running ? fStopIcon : fPlayIcon,
		B_TRIM_ICON_BITMAP_KEEP_ASPECT);
	fStartButton->SetToolTip(running ? "Stop" : "Play");
}

void
MainWindow::_ResetDmbStations()
{
	fSelectedDmbSubchannel = -1;
	fReceiver.SetDabSubchannel(-1);
	fDmbProbing = false;
	fDmbStandby = false;
	BMenu* menu = fServicesView->Menu();
	if (menu != NULL) {
		while (menu->CountItems() > 0)
			delete menu->RemoveItem((int32)0);
		BMenuItem* placeholder = new BMenuItem("Station...", NULL);
		placeholder->SetEnabled(false);
		placeholder->SetMarked(true);
		menu->AddItem(placeholder);
	}
	fServicesView->SetEnabled(false);
	_UpdateTransportVisibility();
}

void
MainWindow::_BeginDmbProbe()
{
	if (fMode != kModeDMB || fDmbProbing || fDmbProbeStopping)
		return;
	fDmbProbing = true;
	fDmbStandby = true;
	fStatusView->SetText("reading station list from FIC...");
	if (!fReceiver.IsRunning()) {
		status_t err = fReceiver.Start();
		if (err != B_OK) {
			fDmbProbing = false;
			fStatusView->SetText("could not read station list");
		}
	}
}

int32
MainWindow::_StopDmbProbeEntry(void* cookie)
{
	MainWindow* self = static_cast<MainWindow*>(cookie);
	self->fReceiver.Stop();
	BMessenger(self).SendMessage(kMsgDmbProbeStopped);
	return B_OK;
}

void
MainWindow::_ToggleRunning()
{
	if (fMode == kModeDMB && fSelectedDmbSubchannel < 0)
		return;
	if (fMode == kModeDMB && fDmbStandby && fReceiver.IsRunning()) {
		// Keep the synchronous FIC USB stream alive and promote it to MSC
		// decoding. Stop followed by Start corrupts Haiku's libusb queue.
		fReceiver.SetDabSubchannel(fSelectedDmbSubchannel);
		fDmbStandby = false;
		_SetTransportRunning(true);
		fStatusView->SetText("starting selected T-DMB station...");
		return;
	}
	if (fReceiver.IsRunning()) {
		fReceiver.Stop();
		_SetTransportRunning(false);
		fStatusView->SetText("stopped");
		return;
	}
	if (fMode == kModeDMB)
		fReceiver.SetDabSubchannel(fSelectedDmbSubchannel);

	status_t err = fReceiver.Start();
	if (err != B_OK) {
		Receiver::snapshot snap;
		fReceiver.Fetch(snap);
		BString text("cannot start: ");
		text << snap.status.c_str();
		fStatusView->SetText(text.String());
		return;
	}
	_SetTransportRunning(true);

	BString text("device: ");
	text << fReceiver.DeviceDescription().c_str();
	fStatusView->SetText(text.String());
}

void
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgFrequency:
			_ApplyFrequencyFromField();
			if (fMode == kModeDMB) {
				_ResetDmbStations();
				_BeginDmbProbe();
			}
			break;

		case kMsgMode:
		{
			int32 mode = 0;
			if (message->FindInt32("mode", &mode) == B_OK)
				_SelectMode((demod_mode)mode);
			break;
		}

		case kMsgPreset:
		{
			int32 index = 0;
			if (message->FindInt32("preset", &index) == B_OK) {
				const preset& p = PresetAt(index);
				_SelectMode(p.mode, false);
				_SetFrequency(p.frequencyHz);
				fReceiver.SetFineTune(0.0f);
				if (p.mode == kModeDMB) {
					_ResetDmbStations();
					_BeginDmbProbe();
				}
			}
			break;
		}

		case kMsgStereo:
			fReceiver.SetFmStereo(
				fStereoBox->Value() == B_CONTROL_ON);
			break;

		case kMsgSquelch:
		{
			int db = fSquelchSlider->Value();
			fReceiver.SetSquelchDb(db <= -100 ? -200.0f : (float)db);
			break;
		}

		case kMsgVolume:
		{
			int value = fVolumeSlider->Value();
			fReceiver.SetVolume((float)value / 100.0f);
			break;
		}

		case kMsgDmbService:
		{
			int32 subchannel;
			if (message->FindInt32("subchannel", &subchannel) == B_OK) {
				fSelectedDmbSubchannel = subchannel;
				if (!fDmbStandby)
					fReceiver.SetDabSubchannel(subchannel);
				_UpdateTransportVisibility();
			}
			break;
		}

		case kMsgDmbProbeStopped:
			fDmbProbeStopping = false;
			fDmbProbing = false;
			_SetTransportRunning(false);
			fServicesView->SetEnabled(fServicesView->Menu() != NULL
				&& fServicesView->Menu()->CountItems() > 1);
			fStatusView->SetText("station list ready");
			_UpdateTransportVisibility();
			break;

		case kMsgTick:
			_Tick();
			break;

		case kMsgStartStop:
			_ToggleRunning();
			break;

		case SpectrumView::kMsgSpectrumClicked:
		{
			if (fMode != kModeAM && fMode != kModeWFM && fMode != kModeAir)
				break;
			float offset = 0.0f;
			if (message->FindFloat("offsetHz", &offset) != B_OK)
				break;
			// Clicking the spectrum retunes the tuner rather than just
			// offsetting the demodulator, so the wanted signal ends up in the
			// middle of the passband where the filters are best behaved.
			int64 target = (int64)fFrequency + (int64)offset;
			if (target > 0) {
				_SetFrequency((uint64)target);
				fReceiver.SetFineTune(0.0f);
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}

void
MainWindow::_Tick()
{
	if (!fReceiver.IsRunning())
		return;

	// The analogue trace benefits from the 30 Hz window ticker, but DMB has no
	// spectrum or waveform display.  Formatting all status fields, rebuilding
	// menu state and invalidating the video view at 30 Hz took a material share
	// of the single Atom core and starved the MSC decoder. Four updates a second
	// keep lock/RS telemetry responsive without competing with audio recovery.
	if (fMode == kModeDMB) {
		static bigtime_t sLastDmbUi = 0;
		bigtime_t now = system_time();
		if (sLastDmbUi != 0 && now - sLastDmbUi < 250000)
			return;
		sLastDmbUi = now;
	}

	Receiver::snapshot snap;
	fReceiver.Fetch(snap);
	if (fSignalGauge != NULL)
		fSignalGauge->SetQuality(
			snap.mode == kModeDMB ? snap.dabQuality : -1.0f, snap.signalDb);
	// Haiku's Atom cannot decode H.264 alongside the OFDM/MSC chain. Do not
	// wake the video decoder even for a still frame; the whole core is reserved
	// for the supported ER-BSAC audio path.

	mode_plan plan = PlanForMode(snap.mode);
	fSpectrum->SetTuning(snap.frequencyHz, 0.0f,
		plan.channelBandwidth, snap.mode);
	fSpectrum->SetData(snap.spectrumDb, snap.spectrumRate);
	fWaterfall->SetTuning(snap.frequencyHz, snap.spectrumRate, snap.mode);
	// One waterfall line per actual spectrum update, not per UI frame. Same
	// auto-ranged scale as the trace above it, so the two agree.
	if (!snap.spectrumDb.empty()
		&& snap.spectrumSequence != fLastSpectrumSequence) {
		fLastSpectrumSequence = snap.spectrumSequence;
		fWaterfall->AddLine(snap.spectrumDb, fSpectrum->FloorDb(),
			fSpectrum->CeilDb());
	}

	BString status(snap.status.c_str());
	if (snap.mode == kModeDMB) {
		status.SetToFormat("%s   T-DMB %s   null %.3f   MER %.1f dB"
			"   offset %+.0f Hz   locks %u/%u", snap.status.c_str(),
			snap.dabLocked ? "LOCKED" : "no lock",
			snap.dabNullDepth, snap.dabMerDb, snap.dabOffsetHz,
			(unsigned)snap.dabLocks, (unsigned)snap.dabAnalyses);
	}
	fStatusView->SetText(status.String());

	if (snap.mode == kModeDMB) {
		BString ens;
		if (snap.dabEnsemble.empty()) {
			ens.SetToFormat("ensemble: (waiting for FIC)   FIBs %u/%u",
				(unsigned)snap.dabFibsOk, (unsigned)snap.dabFibsTried);
		} else {
			ens.SetToFormat("ensemble: %s   FIBs %u/%u passing CRC",
				snap.dabEnsemble.c_str(),
				(unsigned)snap.dabFibsOk, (unsigned)snap.dabFibsTried);
		}
		fEnsembleView->SetText(ens.String());

		BMenu* menu = fServicesView->Menu();
		bool rebuild = menu == NULL
			|| (size_t)menu->CountItems() != snap.dabServiceChoices.size() + 1;
		if (!rebuild && menu != NULL) {
			for (size_t i = 0; i < snap.dabServiceChoices.size(); i++) {
				if (menu->ItemAt((int32)i + 1) == NULL
					|| strcmp(menu->ItemAt((int32)i + 1)->Label(),
						snap.dabServiceChoices[i].label.c_str()) != 0) {
					rebuild = true;
					break;
				}
			}
		}
		if (rebuild && menu != NULL) {
			while (menu->CountItems() > 0)
				delete menu->RemoveItem((int32)0);
			BMenuItem* placeholder = new BMenuItem("Station...", NULL);
			placeholder->SetEnabled(false);
			placeholder->SetMarked(fSelectedDmbSubchannel < 0);
			menu->AddItem(placeholder);
			for (size_t i = 0; i < snap.dabServiceChoices.size(); i++) {
				BMessage* select = new BMessage(kMsgDmbService);
				select->AddInt32("subchannel",
					snap.dabServiceChoices[i].subchannel);
				menu->AddItem(new BMenuItem(
					snap.dabServiceChoices[i].label.c_str(), select));
			}
		}
		fServicesView->SetEnabled(!snap.dabServiceChoices.empty()
			&& !fDmbProbeStopping);
		if (menu != NULL) {
			for (int32 i = 0; i < menu->CountItems(); i++) {
				BMenuItem* item = menu->ItemAt(i);
				if (item == NULL)
					continue;
				if (i == 0)
					item->SetMarked(fSelectedDmbSubchannel < 0);
				else
					item->SetMarked(snap.dabServiceChoices[(size_t)i - 1].subchannel
						== fSelectedDmbSubchannel);
			}
		}
		if (fDmbProbing && !snap.dabServiceChoices.empty()) {
			fDmbProbing = false;
			fDmbStandby = true;
			fServicesView->SetEnabled(true);
			fStatusView->SetText("station list ready");
			_SetTransportRunning(false);
		}
	} else {
		fEnsembleView->SetText("");
		fServicesView->SetEnabled(false);
	}

	BString meter;
	meter.SetToFormat("RF %.1f dBFS   gain %.1f%s   peak %.2f   load %.0f%%"
		"   drop %llu   under %llu",
		snap.signalDb, snap.gainTenths / 10.0,
		snap.automaticGain ? "a" : "",
		snap.audioPeak, snap.dspLoad * 100.0,
		(unsigned long long)snap.droppedBlocks,
		(unsigned long long)snap.underruns);
	if (snap.directSampling)
		meter << "   Q direct sampling";
	if (snap.mode == kModeDMB) {
		BString video;
		video.SetToFormat("   video %llu",
			(unsigned long long)fDmbVideo->Frames());
		meter << video;
	}

	// The software AGC aims for -15 dBFS; anything near 0 means the 8-bit ADC
	// is clipping, which sounds like crackle and cannot be filtered out
	// afterwards.
	if (snap.signalDb > -4.0f)
		meter << "   ** ADC CLIPPING - reduce gain **";

	// Frames the sound card has actually taken.
	BString played;
	played.SetToFormat("   out %lluk", (unsigned long long)(snap.framesPlayed / 1000));
	meter << played;

	fMeterView->SetText(meter.String());
}
