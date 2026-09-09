#pragma once
// Fundamental detection by autocorrelation, computed as IFFT(|FFT|^2).
//
// The period is the SHORTEST lag carrying a strong peak. That ordering is the
// whole trick: taking the LARGEST peak instead finds the sub-octave about as
// often as the octave, because a periodic signal correlates just as well at
// twice its period. Harmonic-product spectra have the mirror-image problem and
// need octave hacks bolted on; this needs none.
//
// Lifted from Band, which proved it. Band still carries its own copy because
// its FFT is shared with the display's spectrum and separating the two is a
// change to a shipping module; it should be migrated onto this header when
// something else is being done to it anyway. Until then, a fix here belongs
// there too.

#include <rack.hpp>
#include <cmath>
#include <vector>

namespace sfs {

struct PitchTracker {
	// 4096 AT 48 kHz IS AN 85 ms WINDOW, and the window is what sets the LOW
	// end, not the lag search. Autocorrelation needs several periods inside the
	// window to find one, and the FFT's correlation is circular, so a lag that
	// eats most of the window is measured against very little overlap. At 2048
	// the claimed 40 Hz floor was fiction: 40 Hz gets 1.7 periods and 98 Hz --
	// a low male voice, a bass tape -- came back NOT DETECTED at all, measured.
	// 4096 gives 98 Hz 8.4 periods and detects it to a cent. Below about 55 Hz
	// it is still guessing; that is a window length problem and the honest fix
	// is a longer window, not a wider lag search.
	static const int FFT_N = 4096;
	static const int FFT_BINS = FFT_N / 2;

	rack::dsp::RealFFT* fft = nullptr;
	std::vector<float> ring, win, spec, acfIn, acf;
	int ringPos = 0;
	float f0 = 0.f;
	bool valid = false;

	PitchTracker() {
		fft = new rack::dsp::RealFFT(FFT_N);
		ring.assign(FFT_N, 0.f);
		win.assign(FFT_N, 0.f);
		spec.assign(2 * FFT_N, 0.f);
		acfIn.assign(2 * FFT_N, 0.f);
		acf.assign(2 * FFT_N, 0.f);
		for (int i = 0; i < FFT_N; i++)          // Hann
			win[i] = 0.5f - 0.5f * std::cos(2.f * (float)M_PI * (float)i / (float)(FFT_N - 1));
	}
	~PitchTracker() { delete fft; }

	// Call every sample. Returns true on the frames where a new estimate landed.
	void push(float x) {
		ring[ringPos] = x;
		ringPos = (ringPos + 1) % FFT_N;
	}

	// Analyse a WINDOW OF AN EXISTING BUFFER rather than the live stream, for a
	// caller that already holds the audio -- a tape, a sample, a recorded loop.
	// It wraps at the end, because a loop's window legitimately crosses the seam.
	// The detector is unchanged: only where its input comes from differs.
	void analyseSpan(const float* data, int n, int start, float sr) {
		if (!data || n <= 0) { valid = false; return; }
		for (int i = 0; i < FFT_N; i++) {
			int k = start + i;
			k %= n;
			if (k < 0) k += n;
			ring[i] = data[k];
		}
		ringPos = 0;
		analyse(sr);
	}

	// Call on a timer (Band runs it a few times a second; more often than that
	// buys nothing, since the window is 2048 samples of history either way).
	void analyse(float sr) {
		static std::vector<float> buf;
		buf.resize(FFT_N);
		for (int i = 0; i < FFT_N; i++)
			buf[i] = ring[(ringPos + i) % FFT_N] * win[i];
		fft->rfft(buf.data(), spec.data());

		acfIn[0] = spec[0] * spec[0];
		acfIn[1] = spec[1] * spec[1];
		for (int k = 1; k < FFT_BINS; k++) {
			float re = spec[2 * k], im = spec[2 * k + 1];
			acfIn[2 * k]     = re * re + im * im;
			acfIn[2 * k + 1] = 0.f;
		}
		fft->irfft(acfIn.data(), acf.data());

		int minLag = std::max(2, (int)std::floor(sr / 2000.f));      // up to 2 kHz
		int maxLag = std::min(FFT_N - 2, (int)std::ceil(sr / 40.f)); // down to 40 Hz
		float zero = (acf[0] > 1e-6f) ? acf[0] : 1e-6f;

		// SKIP THE CENTRAL LOBE BEFORE LOOKING FOR A PEAK. Autocorrelation has a
		// maximum at lag 0 by construction, and the lobe around it is as wide as
		// the signal's own correlation time. minLag is set by the HIGHEST pitch
		// worth finding (2 kHz, 24 samples at 48k), and on a low, bright source
		// the lobe is far wider than that -- so the lobe's tail, not the period,
		// was the largest value in the search range. Measured: a 55 Hz tone with
		// fourteen harmonics came back NOT DETECTED, because the winner was lag
		// 24 and the result was then thrown out for being too short. A 98 Hz one
		// failed the same way.
		//
		// Walking past the first point where the correlation has genuinely
		// dropped costs nothing and is what makes the peak that follows a PERIOD
		// rather than the shoulder of lag zero.
		int searchFrom = minLag;
		while (searchFrom < maxLag && acf[searchFrom] > 0.35f * zero) searchFrom++;
		if (searchFrom >= maxLag) { valid = false; return; }

		float gmax = 0.f;
		for (int t = searchFrom; t <= maxLag; t++) if (acf[t] > gmax) gmax = acf[t];

		int bestLag = -1;
		if (gmax > 0.f) {
			float thr = 0.85f * gmax;
			for (int t = searchFrom; t <= maxLag; t++) {
				if (acf[t] >= thr) {
					while (t + 1 <= maxLag && acf[t + 1] > acf[t]) t++;   // climb to the peak
					bestLag = t;
					break;
				}
			}
		}
		if (bestLag > searchFrom && bestLag < maxLag && gmax > 0.05f * zero) {
			// Parabolic interpolation across the peak: without it the estimate
			// is quantized to whole samples of lag, which at 2 kHz is a
			// quarter-tone.
			float a = acf[bestLag - 1], b = acf[bestLag], c = acf[bestLag + 1];
			float denom = a - 2.f * b + c;
			float d = (std::fabs(denom) > 1e-9f) ? 0.5f * (a - c) / denom : 0.f;
			d = rack::math::clamp(d, -0.5f, 0.5f);
			float raw = sr / ((float)bestLag + d);
			// Snap on a note change, smooth otherwise. One filter cannot do
			// both: slow enough to reject jitter is slow enough to glide
			// audibly across a leap.
			if (!valid || std::fabs(raw - f0) > f0 * 0.06f) f0 = raw;
			else f0 += (raw - f0) * 0.3f;
			valid = true;
		} else {
			valid = false;
		}
	}
};

}  // namespace sfs
