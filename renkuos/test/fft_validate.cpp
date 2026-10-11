#include "Dsp.h"
#include <cmath>
#include <cstdio>

int main()
{
	for (int n = 2; n <= 2048; n <<= 1) {
		std::vector<dsp::Cf> x((size_t)n);
		for (int i = 0; i < n; i++) {
			x[i].re = std::sin(0.17 * i) + 0.3f * std::cos(0.41 * i);
			x[i].im = std::cos(0.23 * i) - 0.2f * std::sin(0.37 * i);
		}
		std::vector<dsp::Cf> ref((size_t)n);
		std::vector<dsp::Cf> unordered = x;
		const double pi = 3.14159265358979323846;
		for (int k = 0; k < n; k++) {
			for (int t = 0; t < n; t++) {
				double a = -2.0 * pi * k * t / n;
				double c = std::cos(a), s = std::sin(a);
				ref[k].re += (float)(x[t].re * c - x[t].im * s);
				ref[k].im += (float)(x[t].re * s + x[t].im * c);
			}
		}
		dsp::Fft(&x[0], n);
		float maxErr = 0.0f;
		for (int k = 0; k < n; k++) {
			float er = std::fabs(x[k].re - ref[k].re);
			float ei = std::fabs(x[k].im - ref[k].im);
			if (er > maxErr) maxErr = er;
			if (ei > maxErr) maxErr = ei;
		}
		std::printf("fft n=%d max_error=%.8g\n", n, maxErr);
		if (maxErr > 1e-3f)
			return 1;

		if (dsp::FftBitReversed(&unordered[0], n)) {
			float unorderedMaxErr = 0.0f;
			for (int k = 0; k < n; k++) {
				int xk = k;
				int reversed = 0;
				for (int bit = 1; bit < n; bit <<= 1) {
					reversed = (reversed << 1) | (xk & 1);
					xk >>= 1;
				}
				float er = std::fabs(unordered[reversed].re - ref[k].re);
				float ei = std::fabs(unordered[reversed].im - ref[k].im);
				if (er > unorderedMaxErr) unorderedMaxErr = er;
				if (ei > unorderedMaxErr) unorderedMaxErr = ei;
			}
			std::printf("fft-unordered n=%d max_error=%.8g\n", n,
				unorderedMaxErr);
			if (unorderedMaxErr > 1e-3f)
				return 1;
		}
	}
	return 0;
}
