#!/bin/sh
# Finishes the bundle: Info.plist, and rewrites the dylib load paths so the
# copies inside Contents/Frameworks are used instead of the Homebrew prefix.
set -e
APP="R SDR.app"
BIN="$APP/Contents/MacOS/R SDR"
BREW=$(brew --prefix)

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
	"http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>R SDR</string>
	<key>CFBundleDisplayName</key><string>R SDR</string>
	<key>CFBundleExecutable</key><string>R SDR</string>
	<key>CFBundleIdentifier</key><string>com.wooddadda.rsdr</string>
	<key>CFBundleIconFile</key><string>R SDR</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST

# Copy librtlsdr and everything it needs from Homebrew into the bundle, so the
# app runs on a Mac that has no brew installed.
copy_deps() {
	target="$1"
	for dep in $(otool -L "$target" | awk 'NR>1 {print $1}'); do
		case "$dep" in
			"$BREW"/*|/usr/local/*)
				base=$(basename "$dep")
				if [ ! -e "$APP/Contents/Frameworks/$base" ]; then
					cp "$dep" "$APP/Contents/Frameworks/$base"
					chmod u+w "$APP/Contents/Frameworks/$base"
					copy_deps "$APP/Contents/Frameworks/$base"
				fi
				;;
		esac
	done
}
copy_deps "$BIN"

# Rewrite every reference to a Homebrew path, in the binary and in the copies.
retarget() {
	target="$1"
	for dep in $(otool -L "$target" | awk 'NR>1 {print $1}'); do
		case "$dep" in
			"$BREW"/*|/usr/local/*)
				base=$(basename "$dep")
				install_name_tool -change "$dep" \
					"@executable_path/../Frameworks/$base" "$target" 2>/dev/null || true
				;;
		esac
	done
}
retarget "$BIN"
for lib in "$APP/Contents/Frameworks/"*.dylib; do
	[ -e "$lib" ] || continue
	install_name_tool -id "@executable_path/../Frameworks/$(basename "$lib")" "$lib" 2>/dev/null || true
	retarget "$lib"
done

for lib in "$APP/Contents/Frameworks/"*.dylib; do
	[ -e "$lib" ] || continue
	base=$(basename "$lib")
	install_name_tool -change "$BREW/lib/$base" "@executable_path/../Frameworks/$base" "$BIN" 2>/dev/null || true
	install_name_tool -change "$BREW/opt/librtlsdr/lib/$base" "@executable_path/../Frameworks/$base" "$BIN" 2>/dev/null || true
	install_name_tool -change "$BREW/opt/libusb/lib/$base" "@executable_path/../Frameworks/$base" "$BIN" 2>/dev/null || true
	install_name_tool -id "@executable_path/../Frameworks/$base" "$lib" 2>/dev/null || true
done
# librtlsdr itself loads libusb; point that at the bundled copy too.
for lib in "$APP/Contents/Frameworks/"librtlsdr*.dylib; do
	[ -e "$lib" ] || continue
	for dep in $(otool -L "$lib" | awk 'NR>1 {print $1}' | grep libusb || true); do
		install_name_tool -change "$dep" "@executable_path/../Frameworks/$(basename "$dep")" "$lib" 2>/dev/null || true
	done
done
codesign --force --deep -s - "$APP" 2>/dev/null || true
