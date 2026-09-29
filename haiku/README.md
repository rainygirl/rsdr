# <img src="../icon.png" alt="" width="28" align="top"> R SDR for Haiku

*[한국어](README.ko.md) · [Documentation index](../README.md)*

![R SDR running on Haiku](screenshot.png)

## Installation

R SDR targets the `x86` secondary architecture on Haiku x86_gcc2. Install the
RTL-SDR runtime and development packages:

```sh
pkgman install rtl_sdr_x86 rtl_sdr_x86_devel
```

From the repository root, build the bundled BSAC decoder once and run the
installer:

```sh
setarch x86 make -C third_party/bsac
cd haiku
./install.sh
```

The app is installed at `/boot/home/config/non-packaged/apps/R SDR`. Desktop
and Deskbar Applications links are also created. After the first installation,
one reboot may be required before the Deskbar link appears.

### arm64 (RENKU)

On arm64 there is a ready-built package on pkgman.rainygirl.com, with
librtlsdr, libusb and FFmpeg inside it (the RENKU arm64 image has the
repository already):

```sh
pkgman install rsdr
```

## Usage

1. Connect an RTL2832U-compatible dongle before opening R SDR.
2. Select a Preset, or select a Mode and enter a frequency. AM frequencies are
   entered in kHz; all other modes use MHz.
3. Press Play, then adjust Volume. Squelch is available in AM, Air, LSB, and
   USB modes; move it fully left to disable it.
4. In WFM mode, enable Stereo only when the signal is strong enough for stable
   reception. Click the spectrum or waterfall to tune to that frequency.
5. For T-DMB, select an on-air T-DMB preset and wait while automatic gain
   detection finishes. The Station list may take about 11 seconds or longer to
   appear. Select a station, then press Play. The Haiku build plays T-DMB audio.

Below 28.8 MHz, the app automatically switches to Q-branch direct sampling.
Reception in this range requires a dongle whose Q input is connected to the
antenna or exposed as a separate HF input.

If USB reception stops, stop playback and reconnect the dongle. If the Haiku
USB bulk endpoint remains unresponsive, physically unplug and reconnect it
before starting playback again.

## License

R SDR's original source code is licensed under the [MIT License](../LICENSE).
Bundled components under `third_party/` retain their own licenses and are not
relicensed under MIT. See the [third-party notices](../THIRD_PARTY_NOTICES.md)
for details.

## AI disclosure

Parts of R SDR were developed with the assistance of AI coding tools
(Anthropic's Claude). All code has been reviewed and tested by the author.
