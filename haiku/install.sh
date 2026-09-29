#!/bin/sh
# Builds R SDR and installs it into Deskbar's Applications menu. Run on
# Haiku, from anywhere:
#   ./install.sh
#
# The build MUST go through the "x86" secondary architecture: librtlsdr is
# only packaged for it on an x86_gcc2 system (rtl_sdr_x86 / rtl_sdr_x86_devel
# in HaikuDepot), and the primary gcc 2.95 toolchain can neither see those
# headers nor compile this code. setarch below takes care of it.
#
# The Deskbar handling mirrors rworldradio's install.sh, including its two
# hard-won details: the Applications menu is a directory of symlinks under
# /boot/system/non-packaged/data/deskbar/menu/Applications (packagefs merges
# it into the read-only /boot/system/data/... view), and re-running `ln -sf`
# on an existing correct symlink un-merges it until the next reboot - so the
# link is only touched when it points somewhere else.
set -e

cd "$(dirname "$0")"

if [ ! -e /boot/system/develop/lib/x86/librtlsdr.so ]; then
	echo "install.sh: librtlsdr not found." >&2
	echo "  pkgman install rtl_sdr_x86 rtl_sdr_x86_devel" >&2
	exit 1
fi

setarch x86 make "$@"

BINARY=$(ls -t objects.*-release/rsdr 2>/dev/null | head -1)
if [ -z "$BINARY" ]; then
	BINARY=$(ls -t objects.*/rsdr 2>/dev/null | head -1)
fi
if [ -z "$BINARY" ]; then
	echo "install.sh: no objects.*/rsdr found after build" >&2
	exit 1
fi

BINARY="$PWD/$BINARY"
APP_DIR=/boot/home/config/non-packaged/apps
INSTALLED_APP="$APP_DIR/R SDR"
DESKTOP_LINK="/boot/home/Desktop/R SDR"
DESKBAR_APPS_DIR=/boot/system/non-packaged/data/deskbar/menu/Applications

mkdir -p "$APP_DIR" "$DESKBAR_APPS_DIR" /boot/home/Desktop
cp -f "$BINARY" "$INSTALLED_APP"

# Tracker reads the icon from the file's attributes, not from the resources the
# linker wrote into the binary. mimeset is supposed to copy one to the other,
# but on recent Haiku it no longer sniffs ELF files, so a freshly built app gets
# a blank document icon. resattr does the copy directly.
resattr -O -o "$INSTALLED_APP" "$INSTALLED_APP" 2>/dev/null \
	|| echo "install.sh: resattr failed; the icon may show as a blank document" >&2

DESKBAR_LINK="$DESKBAR_APPS_DIR/R SDR"
if [ "$(readlink "$DESKBAR_LINK" 2>/dev/null)" != "$INSTALLED_APP" ]; then
	ln -sf "$INSTALLED_APP" "$DESKBAR_LINK"
fi
if [ "$(readlink "$DESKTOP_LINK" 2>/dev/null)" != "$INSTALLED_APP" ]; then
	ln -sf "$INSTALLED_APP" "$DESKTOP_LINK"
fi

echo "Installed:"
echo "  $INSTALLED_APP"
echo "  $DESKBAR_LINK -> $INSTALLED_APP"
echo "  $DESKTOP_LINK -> $INSTALLED_APP"
echo
if [ -e "/boot/system/data/deskbar/menu/Applications/R SDR" ]; then
	echo "R SDR should now appear in Deskbar's Applications menu."
else
	echo "NOTE: on a first install you likely need to reboot once before it"
	echo "shows up in Deskbar - the packagefs merge of a newly created"
	echo "non-packaged directory is not picked up live."
fi
