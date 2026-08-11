/*
 * Renders the macOS icon from the same geometry as the Haiku one.
 *
 * haiku/tools/make_icon.py emits HVIF, which macOS cannot read, so the shape
 * is rebuilt here rather than converted: identical isometric projection,
 * identical palette, identical order of faces. Keeping the numbers in step is
 * the whole point - the two apps should not look like cousins.
 *
 * Writes R SDR.iconset/, which iconutil turns into R SDR.icns.
 */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>
#include <vector>

namespace {

struct P { double x, y; };

// The BeOS diamond axes, straight out of make_icon.py.
const P kOrigin = { 32.0, 10.0 };
const P kU = { 26.0, 13.0 };
const P kV = { -26.0, 13.0 };
const double kDepth = 22.0;

P top(double u, double v)
{
	return P{ kOrigin.x + u * kU.x + v * kV.x,
		kOrigin.y + u * kU.y + v * kV.y };
}

P left(double u, double down)
{
	P base = top(0, 1);
	return P{ base.x + u * kU.x, base.y + u * kU.y + down * kDepth };
}

P right(double v, double down)
{
	P base = top(1, 0);
	return P{ base.x + v * kV.x, base.y + v * kV.y + down * kDepth };
}

typedef P (*Projection)(double, double);

std::vector<P> faceRect(Projection p, double a0, double b0, double a1,
	double b1)
{
	std::vector<P> out;
	out.push_back(p(a0, b0));
	out.push_back(p(a1, b0));
	out.push_back(p(a1, b1));
	out.push_back(p(a0, b1));
	return out;
}

std::vector<P> faceDisc(Projection p, double ca, double cb, double ra,
	double rb, int count)
{
	std::vector<P> out;
	for (int i = 0; i < count; i++) {
		double t = 2.0 * M_PI * i / count;
		out.push_back(p(ca + ra * cos(t), cb + rb * sin(t)));
	}
	return out;
}

const double kPalette[10][4] = {
	{ 232 / 255.0, 237 / 255.0, 240 / 255.0, 1.0 },	// bright silver top
	{ 172 / 255.0, 181 / 255.0, 187 / 255.0, 1.0 },	// silver left face
	{ 112 / 255.0, 123 / 255.0, 131 / 255.0, 1.0 },	// silver right face
	{  38 / 255.0,  44 / 255.0,  48 / 255.0, 1.0 },	// speaker panel
	{  11 / 255.0,  18 / 255.0,  22 / 255.0, 1.0 },	// grille holes
	{  31 / 255.0,  91 / 255.0, 119 / 255.0, 1.0 },	// blue display
	{ 104 / 255.0, 221 / 255.0, 255 / 255.0, 1.0 },	// display line
	{ 244 / 255.0, 247 / 255.0, 248 / 255.0, 1.0 },	// dial highlight
	{  74 / 255.0,  83 / 255.0,  90 / 255.0, 1.0 },	// dial shadow / antenna
	{  43 / 255.0,  49 / 255.0,  54 / 255.0, 1.0 },	// feet / outline accents
};

void fill(CGContextRef ctx, int style, const std::vector<P>& pts, double scale)
{
	if (pts.empty())
		return;
	const double* c = kPalette[style];
	CGContextSetRGBFillColor(ctx, c[0], c[1], c[2], c[3]);
	CGContextBeginPath(ctx);
	// The icon grid is y-down; the bitmap context is y-up.
	CGContextMoveToPoint(ctx, pts[0].x * scale, (64.0 - pts[0].y) * scale);
	for (size_t i = 1; i < pts.size(); i++)
		CGContextAddLineToPoint(ctx, pts[i].x * scale, (64.0 - pts[i].y) * scale);
	CGContextClosePath(ctx);
	CGContextFillPath(ctx);
}

void draw(CGContextRef ctx, double scale)
{
	// Black ground. macOS draws app icons on whatever the user's wallpaper is,
	// and the silver cabinet loses its edges against a light one - the Haiku
	// desktop has a fixed grey behind it, so the HVIF can afford transparency
	// and this cannot. Rounded rather than square because that is the platform
	// convention; the radius is the ~22% Big Sur uses.
	{
		double side = 64.0 * scale;
		double inset = side * 0.055;
		double radius = side * 0.2237;
		CGRect r = CGRectMake(inset, inset, side - 2 * inset,
			side - 2 * inset);
		CGPathRef path = CGPathCreateWithRoundedRect(r, radius, radius, NULL);
		CGContextSetRGBFillColor(ctx, 0.04, 0.05, 0.06, 1.0);
		CGContextAddPath(ctx, path);
		CGContextFillPath(ctx);
		CGPathRelease(path);
	}

	// Antenna first, so the cabinet covers its root.
	P root = top(0.18, 0.18);
	std::vector<P> antenna;
	antenna.push_back(P{ root.x - 1, root.y });
	antenna.push_back(P{ root.x + 1, root.y + 1 });
	antenna.push_back(P{ 7, 2 });
	antenna.push_back(P{ 5, 1 });
	fill(ctx, 8, antenna, scale);

	std::vector<P> face;
	face.clear();
	face.push_back(top(0, 1)); face.push_back(top(1, 1));
	face.push_back(left(1, 1)); face.push_back(left(0, 1));
	fill(ctx, 1, face, scale);

	face.clear();
	face.push_back(top(1, 0)); face.push_back(top(1, 1));
	face.push_back(right(1, 1)); face.push_back(right(0, 1));
	fill(ctx, 2, face, scale);

	face.clear();
	face.push_back(top(0, 0)); face.push_back(top(1, 0));
	face.push_back(top(1, 1)); face.push_back(top(0, 1));
	fill(ctx, 0, face, scale);

	// Recessed blue display and two metallic tuning controls.
	fill(ctx, 8, faceRect(left, 0.08, 0.13, 0.91, 0.43), scale);
	fill(ctx, 5, faceRect(left, 0.13, 0.17, 0.86, 0.38), scale);
	fill(ctx, 6, faceRect(left, 0.24, 0.25, 0.75, 0.29), scale);
	fill(ctx, 8, faceDisc(left, 0.29, 0.70, 0.16, 0.18, 24), scale);
	fill(ctx, 7, faceDisc(left, 0.29, 0.70, 0.12, 0.14, 24), scale);
	fill(ctx, 8, faceDisc(left, 0.69, 0.70, 0.12, 0.15, 24), scale);
	fill(ctx, 7, faceDisc(left, 0.69, 0.70, 0.08, 0.10, 20), scale);

	// Speaker grille with silver slats.
	fill(ctx, 3, faceRect(right, 0.09, 0.12, 0.91, 0.88), scale);
	for (int row = 0; row < 5; row++) {
		double y = 0.18 + row * 0.14;
		fill(ctx, 4, faceRect(right, 0.16, y, 0.84, y + 0.055), scale);
	}

	// Handle and feet.
	fill(ctx, 2, faceRect(top, 0.23, 0.40, 0.77, 0.60), scale);
	fill(ctx, 9, faceRect(left, 0.08, 0.96, 0.23, 1.12), scale);
	fill(ctx, 9, faceRect(right, 0.08, 0.96, 0.23, 1.12), scale);
}

bool writePng(const char* path, int pixels)
{
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(NULL, pixels, pixels, 8,
		(size_t)pixels * 4, cs, kCGImageAlphaPremultipliedLast);
	CGColorSpaceRelease(cs);
	if (ctx == NULL)
		return false;
	CGContextSetAllowsAntialiasing(ctx, true);
	CGContextClearRect(ctx, CGRectMake(0, 0, pixels, pixels));
	draw(ctx, pixels / 64.0);

	CGImageRef img = CGBitmapContextCreateImage(ctx);
	CGContextRelease(ctx);
	if (img == NULL)
		return false;

	CFStringRef p = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
	CFURLRef url = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle,
		false);
	CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url,
		CFSTR("public.png"), 1, NULL);
	bool ok = false;
	if (dst != NULL) {
		CGImageDestinationAddImage(dst, img, NULL);
		ok = CGImageDestinationFinalize(dst);
		CFRelease(dst);
	}
	CFRelease(url);
	CFRelease(p);
	CGImageRelease(img);
	return ok;
}

} // namespace

int
main(int argc, const char** argv)
{
	@autoreleasepool {
		const char* dir = argc > 1 ? argv[1] : "R SDR.iconset";
		[[NSFileManager defaultManager]
			createDirectoryAtPath:[NSString stringWithUTF8String:dir]
			withIntermediateDirectories:YES attributes:nil error:nil];

		struct { const char* name; int px; } wanted[] = {
			{ "icon_16x16.png", 16 },      { "icon_16x16@2x.png", 32 },
			{ "icon_32x32.png", 32 },      { "icon_32x32@2x.png", 64 },
			{ "icon_128x128.png", 128 },   { "icon_128x128@2x.png", 256 },
			{ "icon_256x256.png", 256 },   { "icon_256x256@2x.png", 512 },
			{ "icon_512x512.png", 512 },   { "icon_512x512@2x.png", 1024 },
		};
		for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
			char path[512];
			snprintf(path, sizeof(path), "%s/%s", dir, wanted[i].name);
			if (!writePng(path, wanted[i].px)) {
				fprintf(stderr, "failed writing %s\n", path);
				return 1;
			}
		}
		printf("wrote %s\n", dir);
	}
	return 0;
}
