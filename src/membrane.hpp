#pragma once
// Modal synthesis of a struck circular membrane, for Kit.
//
// A drum head is a 2D wave equation on a disc, and its solutions are the Bessel
// modes J_m(j_mn * r/R) * cos(m*theta). Rather than run a mesh over the disc,
// this bank runs one resonator per mode. That is cheaper, and much more
// importantly it puts every parameter you would want to play in plain sight:
// strike position becomes a per-mode gain you can evaluate in closed form, and
// muffling becomes the same evaluation used subtractively.
//
// THE ONE THING TO KNOW ABOUT THE PARAMETERS. For an ideal membrane
// f = (c / 2*pi*R) * j_mn with c = sqrt(T/sigma), so radius and tension scale
// every mode by the same factor and leave the RATIOS untouched. Size and
// tightness are therefore the same control, acoustically, and shipping both as
// separate knobs would be shipping a duplicate. What breaks the scale
// invariance -- and so what makes a small tight drum sound different from a
// large slack one at the same pitch -- is bending stiffness, the air cavity and
// size-dependent damping. Those are not decoration here; they are the reason
// SIZE is allowed to exist.

#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace sfs {

// ── the mode table ──────────────────────────────────────────────────────────
// j_mn, the zeros of J_m, sorted by frequency. Computed offline by bisection on
// the integral form of J_m and checked against the textbook series
// 1, 1.594, 2.136, 2.296, 2.653, 2.918, 3.156, 3.501 -- which it reproduces to
// three decimals. (The first attempt at generating it scanned upward from zero
// and "found" roots at x = 0.00002, because J_m(0) = 0 for m >= 1 and rounding
// noise flips the sign down there. J_m has no zeros below x = m.)
struct MembraneMode { int m, n; float j; };

static const MembraneMode MEMBRANE_MODES[] = {
	{ 0, 1,  2.404826f},	{ 1, 1,  3.831706f},	{ 2, 1,  5.135622f},
	{ 0, 2,  5.520078f},	{ 3, 1,  6.380162f},	{ 1, 2,  7.015587f},
	{ 4, 1,  7.588342f},	{ 2, 2,  8.417244f},	{ 0, 3,  8.653728f},
	{ 5, 1,  8.771484f},	{ 3, 2,  9.761023f},	{ 6, 1,  9.936110f},
	{ 1, 3, 10.173468f},	{ 4, 2, 11.064709f},	{ 7, 1, 11.086370f},
	{ 2, 3, 11.619841f},	{ 0, 4, 11.791534f},	{ 8, 1, 12.225092f},
	{ 5, 2, 12.338604f},	{ 3, 3, 13.015201f},	{ 1, 4, 13.323692f},
	{ 9, 1, 13.354300f},	{ 6, 2, 13.589290f},	{ 4, 3, 14.372537f},
	{10, 1, 14.475501f},	{ 2, 4, 14.795952f},	{ 7, 2, 14.821269f},
	{ 0, 5, 14.930918f},	{11, 1, 15.589848f},	{ 5, 3, 15.700174f},
	{ 8, 2, 16.037774f},	{ 3, 4, 16.223466f},	{ 1, 5, 16.470630f},
	{12, 1, 16.698250f},	{ 6, 3, 17.003820f},	{ 9, 2, 17.241220f},
	{ 4, 4, 17.615966f},	{13, 1, 17.801435f},	{ 2, 5, 17.959819f},
	{ 0, 6, 18.071064f},
};
static const int MEMBRANE_NMODES = (int)(sizeof(MEMBRANE_MODES) / sizeof(MEMBRANE_MODES[0]));

// ── J_m, for the spatial tables ─────────────────────────────────────────────
// The integral form. Slow, but it runs once at startup to fill a table and it
// cannot suffer the cancellation that the ascending series does at large x.
static inline float besselJ(int m, float x) {
	const int N = 256;
	double s = 0.0, h = M_PI / N;
	for (int i = 0; i <= N; i++) {
		double t = i * h, w = (i == 0 || i == N) ? 0.5 : 1.0;
		s += w * std::cos(m * t - (double)x * std::sin(t));
	}
	return (float)(s * h / M_PI);
}

// J_m(j_mn * u) for u = r/R in 0..1, per mode. This is the radial part of a
// mode's shape, and it answers two questions with the same numbers: how hard a
// strike at radius r drives the mode, and how much a muffle at radius r damps
// it. Built once and shared by every instance.
static const int MEMBRANE_NRAD = 65;
struct MembraneShapes {
	float tab[64 /*>= MEMBRANE_NMODES*/][MEMBRANE_NRAD];
	MembraneShapes() {
		for (int k = 0; k < MEMBRANE_NMODES; k++)
			for (int i = 0; i < MEMBRANE_NRAD; i++)
				tab[k][i] = besselJ(MEMBRANE_MODES[k].m,
				                    MEMBRANE_MODES[k].j * (float)i / (MEMBRANE_NRAD - 1));
	}
	// Radial shape of mode k at u = r/R, linearly interpolated.
	inline float at(int k, float u) const {
		if (u < 0.f) u = 0.f;
		if (u > 1.f) u = 1.f;
		float f = u * (MEMBRANE_NRAD - 1);
		int i = (int)f;
		if (i >= MEMBRANE_NRAD - 1) return tab[k][MEMBRANE_NRAD - 1];
		float t = f - i;
		return tab[k][i] + t * (tab[k][i + 1] - tab[k][i]);
	}
};
static inline const MembraneShapes& membraneShapes() {
	static MembraneShapes s;      // built on first use, then shared
	return s;
}

// ── one mode ────────────────────────────────────────────────────────────────
// A rotating (coupled-form) resonator rather than a biquad. Tension modulation
// moves every mode's frequency continuously while it is ringing, and a
// direct-form biquad re-tuned under those conditions rings badly or blows up.
// A rotation just changes its rate: the state stays exactly as valid as it was,
// so there is no click and no stability question to answer.
struct ModeOsc {
	float re = 0.f, im = 0.f;
	float cw = 1.f, sw = 0.f, r = 0.f;

	inline void setFreq(float f, float sr) {
		float w = 2.f * (float)M_PI * f / sr;
		if (w > 3.0f) w = 3.0f;             // stay well short of Nyquist
		cw = std::cos(w); sw = std::sin(w);
	}
	inline void setDecay(float t60, float sr) {
		if (t60 < 1e-4f) t60 = 1e-4f;
		r = std::exp(-6.907755f / (t60 * sr));   // 60 dB in t60 seconds
	}
	inline void hit(float amp) { re += amp; }
	inline float process() {
		float nre = r * (cw * re - sw * im);
		float nim = r * (sw * re + cw * im);
		re = nre; im = nim;
		return re;
	}
	inline float value() const { return re; }
	inline void clear() { re = im = 0.f; }
};

// ── the mallet ──────────────────────────────────────────────────────────────
// A mass meeting the head through a Hertzian contact spring, F = k * c^alpha.
// This is one model that answers four separate requirements at once, which is
// why it is worth its cost over a windowed impulse:
//
//   * contact time falls out of the collision, so a harder hit is a shorter
//     contact and therefore brighter, with no velocity-to-brightness mapping
//     invented by hand;
//   * the mallet reads the head's CURRENT displacement, so striking a head that
//     is already moving is a genuinely different collision -- which is the
//     whole of "model the impact of previous reverberations";
//   * it rebounds, so double strokes and press rolls emerge instead of being
//     sequenced;
//   * alpha and mass span stick to soft mallet as a continuum.
struct Mallet {
	bool  active = false;
	float p = 0.f, v = 0.f;      // position and velocity, positive = into the head
	float mass = 1.f, stiff = 1e5f, expo = 1.5f, grav = 0.f;

	inline void strike(float vel, float gap) { active = true; p = -gap; v = vel; }

	// u = head displacement at the strike point. Returns the force to inject.
	inline float process(float u, float dt) {
		if (!active) return 0.f;
		v += grav * dt;                       // press: the stick is held down
		p += v * dt;
		float c = p - u;                      // compression
		if (c > 0.f) {
			float F = stiff * std::pow(c, expo);
			v -= (F / mass) * dt;
			return F;
		}
		if (v < 0.f && p < u - 0.0008f) active = false;   // flew clear
		return 0.f;
	}
	inline void reset() { active = false; p = v = 0.f; }
};

// ── snare wires ─────────────────────────────────────────────────────────────
// Wires lying against the resonant head. They are not driven by it so much as
// thrown off it: below a threshold they stay in contact and do nothing, and
// above it they leave the head and rattle. That threshold is why a snare drum
// answers a loud hit completely differently from a quiet one, and why ghost
// notes sound like a different instrument rather than a quieter one.
struct SnareWires {
	float env = 0.f, buzz = 0.f, lp = 0.f, hp = 0.f, prev = 0.f;
	// The seed is per instance because a stereo pair runs two of these. Twenty
	// strands are not one noise source: two mics over the snare hear different
	// strands, so the two must not share a stream or the buzz collapses to the
	// centre while the drum around it is wide.
	uint32_t rng = 0x9e3779b9u;

	inline float noise() {
		rng = rng * 1664525u + 1013904223u;
		return (float)(rng >> 8) / 8388608.f - 1.f;
	}
	// head = resonant-head signal, thr = lift-off threshold, tight = decay.
	inline float process(float head, float thr, float tight, float sr) {
		float d = head - prev; prev = head;         // wires answer to velocity
		float drive = std::fabs(d) * 400.f;
		float over = drive - thr;
		if (over > 0.f) env += (over - env) * 0.5f;
		env -= env * (1.f / (tight * sr));
		if (env < 0.f) env = 0.f;
		buzz = noise() * env;
		// a bright band: the wires are small and stiff
		float c = 0.35f;
		lp += (buzz - lp) * c;
		hp = buzz - lp;
		return hp;
	}
	inline void clear() { env = buzz = lp = hp = prev = 0.f; }
};


// ── the drum ────────────────────────────────────────────────────────────────
// Two heads, coupled through the shell air, plus wires and a mallet.
//
// ON STRIKE ANGLE, which is easy to get wrong. A circular drum is rotationally
// symmetric, so the angle of an isolated strike CANNOT change its timbre --
// only the radius can. Angle becomes audible solely against something that
// breaks the symmetry, and there is exactly one such thing here: the muffle.
// So radius is the tone control, and angle decides how much the muffle is in
// the way of what you just excited. Making angle change the timbre on its own
// would be inventing physics the drum does not have.
struct Drum {
	static const int NM = MEMBRANE_NMODES;
	ModeOsc lo[NM], hi[NM];        // the two coupled-mode branches per index
	float gain[NM] = {0.f};
	float t60lo[NM] = {0.f}, t60hi[NM] = {0.f};
	float ratio[NM] = {0.f};
	Mallet mallet;
	SnareWires wires[2];
	// The two wire sets must not share a noise stream -- see SnareWires.
	Drum() { wires[1].rng = 0x85ebca6bu; }

	float sr = 48000.f;
	// set by the host
	float f0 = 90.f;          // fundamental, from size and tension together
	float stiff = 0.f;        // 0 = membrane, 1 = plate/gong
	float air = 0.f;          // 0 = ideal (tom), 1 = air-loaded (timpani)
	float couple = 0.35f;     // batter/resonant coupling through the cavity
	float resoTune = 1.f;     // resonant head tension, relative to the batter
	float decay = 1.2f;       // t60 of the fundamental, seconds
	float tone = 0.5f;        // how much faster the high modes die
	float muffle = 0.f, muffleAng = 0.f;
	float strikeR = 0.55f, strikeAng = 0.f;
	float bend = 0.f;         // tension modulation depth
	float snareAmt = 0.f, snareThr = 0.25f, snareTight = 0.25f;

	float energy = 0.f, headDisp = 0.f;

	// ── the stereo pair ─────────────────────────────────────────────────────
	// WHERE THE DRUM IS LISTENED TO, as against where it is hit. In mono the
	// bank taps the head at the strike point, which is why gain[] serves at both
	// ends. Two taps at two places is what a stereo pair over a real drum is,
	// and it costs one extra number per mode: mode k reads
	// J_m(j_mn * r_mic) * cos(m * (theta_mic - theta_strike)), the same closed
	// form the strike and the muffle already use.
	//
	// It is also the first thing here that makes the strike ANGLE audible. A
	// disc is rotationally symmetric, so with ONE listening point the angle
	// cannot matter and only the radius is a tone control (see the note above
	// Drum). A second point breaks the symmetry, so in stereo mode moving the
	// strike round the head sweeps the image -- with no panner anywhere.
	//
	// NOT MIRRORED, deliberately, and this is the whole of why the positions
	// look arbitrary. cos(m * dtheta) is EVEN, so a pair mirrored about some
	// axis returns exactly the same signal at both taps for any strike ON that
	// axis -- and Kit's default strike is straight up the head. A tidy mirrored
	// pair would therefore put the factory patch on the one line where stereo
	// mode collapses to mono. Asymmetric in angle AND radius, the only place the
	// two channels agree is the dead centre, where the strike really is
	// rotationally symmetric and mono really is the right answer.
	// BOTH AT THE SAME RADIUS, which is not tidiness. An m = 0 mode is a
	// monopole: it has no angular shape, so it reads IDENTICALLY at every point
	// on the head and no placement can ever decorrelate it. Two taps at
	// different radii therefore do not separate the monopoles, they only make
	// one channel louder than the other -- measured at 2.8 dB of permanent
	// lean, for nothing. Equal radii, and every difference between the channels
	// is angular.
	//
	// And the monopoles are most of the sound: measured over the ten
	// instruments, 69-93% of the energy sits at m = 0, so the head alone is
	// worth about -12 dB of side, with the fundamental dead centre. That is
	// honest -- a drum's low thump IS mono in a real pair -- but it is not an
	// image, which is what the air below is for.
	// 150 degrees apart, with the pair's centre line 30 degrees off the default
	// strike. Both halves of that are measurements, not taste (harness mode
	// `grid`): the separation is what sets the width, and it flattens out past
	// about 150; the offset decides where the pair's NULL falls. Two points on
	// a disc always have a perpendicular bisector, and a strike along it is
	// equidistant from both mics AND angularly symmetric between them, so it
	// reads identically at each -- there are always two directions on the head
	// where the drum is dead centre. That is a position like any other, but it
	// must not be the one every instrument here ships pointing at. 30 degrees
	// off puts the factory strike 60 degrees clear of it: audibly stereo out of
	// the box, and only 3 dB off centre.
	float micR[2]   = {0.60f, 0.60f};
	float micAng[2] = {3.40f, 0.79f};         // radians; cos < 0 is screen-left
	float tap[2][NM] = {{0.f}};

	// ── the mics are in the AIR, not on the skin ────────────────────────────
	// The taps above are the head's motion under each mic, and on their own
	// they are a pair of contact pickups: no arrival time, no distance. What
	// makes a spaced pair over a real drum wide is that the strike is somewhere
	// on the head and the two mics are at different distances FROM IT -- a few
	// tenths of a millisecond apart, and a little different in level. On a
	// struck instrument that transient cue is most of what the ear localises
	// with, and it is the half of the image the monopoles can carry.
	//
	// It also means the drum MOVES when you move the strike, which nothing else
	// in Kit does and which no panner could fake: hit the left of the head and
	// the drum is on the left, in both channels' timing.
	//
	// This is the one place the drum's ABSOLUTE size matters. Everything else
	// here is scale-invariant (see the top of the file) -- but the speed of
	// sound is not a ratio, so a 22-inch kick images nearly four times as wide
	// as a 6-inch splash. radiusM is metres, set from SIZE by the host.
	float radiusM = 0.14f;
	// How high the mics sit, in radii. It is the level half of the image and
	// almost none of the width: from 0.3 to 1.5 radii the swing across the head
	// goes from 11 dB to under 2, while the side energy moves by half a
	// decibel. 0.7 gives a drum that travels about 5 dB corner to corner.
	float micH = 0.70f;
	static const int MICDL = 512;     // ~2.6 ms at 192k; the span is under 1 ms
	float dline[2][2][MICDL] = {{{0.f}}};   // [channel][head, snare]
	int   dwrite = 0;
	// Latched at the STRIKE, and read through a short crossfade. Sliding them
	// would be a doppler on whatever is still ringing; jumping them without the
	// fade would be a splice, which a fresh transient masks in almost every
	// case but not under a quiet stroke on a loud tail.
	float micDelay[2] = {0.f, 0.f}, micLvl[2] = {1.f, 1.f};
	float micDelayOld[2] = {0.f, 0.f}, micLvlOld[2] = {1.f, 1.f};
	float micXfade = 1.f;             // 1 = settled on the new geometry
	float micXrate = 1.f;

	// Distance from the strike point to each mic, and what that does to the
	// two channels: the nearer mic is the reference, so one channel is always
	// undelayed and at unity and Kit stays a zero-latency module.
	void updateMics() {
		float sx = strikeR * std::cos(strikeAng), sy = strikeR * std::sin(strikeAng);
		float d[2];
		for (int c = 0; c < 2; c++) {
			float dx = micR[c] * std::cos(micAng[c]) - sx;
			float dy = micR[c] * std::sin(micAng[c]) - sy;
			d[c] = radiusM * std::sqrt(dx * dx + dy * dy + micH * micH);
		}
		float dmin = std::min(d[0], d[1]);
		for (int c = 0; c < 2; c++) {
			micDelayOld[c] = micDelay[c]; micLvlOld[c] = micLvl[c];
			micDelay[c] = std::min((float)(MICDL - 2), (d[c] - dmin) / 343.f * sr);
			micLvl[c]   = dmin / d[c];              // 1/r, normalised to the near mic
		}
		micXfade = 0.f;
		micXrate = 1.f / std::max(1.f, 0.002f * sr);   // 2 ms
	}
	// EXCITATION TILT, the thing this bank was missing. A force impulse gives a
	// mode initial VELOCITY, not displacement, so its amplitude goes as
	// J*phi(x)/(m*omega) -- a 6 dB/octave rolloff. Injecting the force straight
	// into displacement, as this did, leaves every high mode that much too loud,
	// which is most of why the whole range leaned metallic.
	//
	// gain[] is used at BOTH ends -- to inject the strike and as the output tap,
	// since Kit listens where it is hit -- so a tilt of 1 here is 1/omega twice
	// and 12 dB/octave in the result. Physics asks for one; the ear asked for
	// two, and the ear was auditioned against the alternatives before this was
	// written down. 0.5 is the physically exact setting if it is ever wanted.
	float tilt = 1.f;
	// The bank works in metres, where a hard hit moves the head about half a
	// millimetre, so it needs taking up to modular level on the way out.
	//
	// This was 2600, calibrated against a test that passed weight = 0.06 when
	// the WEIGHT knob's default is 1.0 -- a beater sixteen times too light. Every
	// preset therefore ran four to sixty times into the soft clip, and the ones
	// that were reported as "distorted" were only the densest, not the only ones.
	float outGain = 420.f;
	// A heavy soft beater really does deliver more momentum than a light stick,
	// so EXCITER genuinely changes loudness -- but 7 dB across a knob whose job
	// is timbre is too much of it. Compensated back to about 2 dB.
	float beaterComp = 1.f;
	// Newtons of contact force to metres of modal displacement. The mallet and
	// the head MUST share a unit: the first version left the head in arbitrary
	// modal units, about 200x larger than the mallet's approach distance, so the
	// head kept slapping back into the stick and a single hit registered up to
	// 296 separate contacts lasting 38 ms in total. A drumstick is ~1 ms.
	float fimp = 2.0e-7f;

	// Mode layout. Called at control rate: everything here is cheap except the
	// pow/sqrt, and none of it needs to be sample-accurate.
	void updateModes() {
		const MembraneShapes& sh = membraneShapes();
		const float j01 = MEMBRANE_MODES[0].j, j11 = MEMBRANE_MODES[1].j;
		float bend0 = 1.f + bend * energy;
		for (int k = 0; k < NM; k++) {
			const MembraneMode& M = MEMBRANE_MODES[k];
			float base = M.j / j01;
			// AIR: the cavity pulls the (m,1) modes into 1 : 1.5 : 2 : 2.5 : 3
			// about the (1,1), which is why a kettledrum has a pitch and a tom
			// does not. Modes outside that family keep their ideal ratio.
			if (M.n == 1 && M.m >= 1) {
				float timp = 0.5f * (M.m + 1) * (j11 / j01);
				base += (timp - base) * air;
			}
			// STIFFNESS: f ~ k*sqrt(T + D*k^2), normalised so the fundamental
			// stays put. At the top the ratios go as k^2 and it is a gong.
			// Ratios go as k^2 at the top, which is a plate rather than a
			// membrane. B has to reach well past 1 for that: at 0.06 the whole
			// knob moved the spectral centroid by 15 Hz, which is no control.
			float B = stiff * stiff * 3.2f;
			float memb = base;          // the membrane ratio, kept for damping
			base *= std::sqrt(1.f + B * base * base) / std::sqrt(1.f + B);
			ratio[k] = base;

			float fb = f0 * base * bend0;
			float fr = fb * resoTune;
			// Two heads sharing a cavity: each mode splits in two. Only the
			// m = 0 modes actually change the cavity volume, so they couple
			// hardest; the rest do it weakly through the near field.
			float w1 = fb, w2 = fr;
			float K = couple * w1 * w1 * (M.m == 0 ? 1.f : 0.3f);
			float a = w1 * w1, b = w2 * w2;
			float half = 0.5f * (a + b + 2.f * K);
			float disc = std::sqrt(std::max(0.f, 0.25f * (a - b) * (a - b) + K * K));
			float wlo = std::sqrt(std::max(1.f, half - disc));
			float whi = std::sqrt(std::max(1.f, half + disc));
			lo[k].setFreq(wlo, sr);
			hi[k].setFreq(whi, sr);

			// Damping. High modes die first; the m = 0 monopoles dump energy
			// into the room fastest, and air loading makes that worse -- which
			// is also why the timpani's (0,1) gets out of the way of its pitch.
			// Damping reads the MEMBRANE ratio, not the stiffened one. Reading
			// the stiffened ratio made STIFFNESS darker instead of brighter: it
			// pushes the high modes up by more than an octave, the
			// frequency-dependent damping then killed them instantly, and the
			// gong lost the shimmer that is the entire point of it. Stiffness
			// here also stands in for metal rather than mylar, and metal has far
			// less internal loss, so it rings longer as well.
			float t = decay * std::pow(memb, -tone * 1.6f) * (1.f + stiff * 3.f);
			// The m = 0 modes are monopoles: they move the whole head one way and
			// shove air, so they always lose energy fastest. This is why a strike
			// dead in the centre -- which excites nothing else -- is a dull thud
			// rather than a note, and it has to be true with the cavity out of
			// the picture, not only when AIR is up.
			if (M.m == 0) t /= (1.7f + air * 3.f);
			// The muffle, using the very same radial shape as the strike: a
			// mode with a belly under your hand loses; a mode with a node there
			// does not notice.
			float ms = sh.at(k, muffle > 0.f ? 0.82f : 0.f);
			float align = 0.5f + 0.5f * std::cos(M.m * (strikeAng - muffleAng));
			t /= (1.f + muffle * 9.f * ms * ms * align);
			t60lo[k] = t;
			t60hi[k] = t * 0.8f;
			lo[k].setDecay(t60lo[k], sr);
			hi[k].setDecay(t60hi[k], sr);
		}
	}

	// Where the strike lands, and where the drum is heard from. Radius alone
	// decides the strike, for the reason at the top; the taps are the one place
	// the angle enters.
	// Call AFTER updateModes(): the tilt needs ratio[], which that computes.
	void updateStrike() {
		const MembraneShapes& sh = membraneShapes();
		float r0 = ratio[0] > 1e-6f ? ratio[0] : 1.f;
		for (int k = 0; k < NM; k++) {
			// The excitation tilt applies ONCE here and once again wherever the
			// signal is tapped -- gain[] injects and, in mono, also listens, so
			// the total is tilt twice. The stereo taps carry exactly one tilt
			// each for that reason: switching to stereo must not change the
			// spectral slope, only where it is heard from.
			float t = 1.f;
			if (tilt > 0.f && ratio[k] > 1e-6f)
				t = std::pow(r0 / ratio[k], tilt);
			gain[k] = sh.at(k, strikeR) * t;
			int m = MEMBRANE_MODES[k].m;
			for (int c = 0; c < 2; c++)
				tap[c][k] = sh.at(k, micR[c]) * t
				          * (m ? std::cos(m * (micAng[c] - strikeAng)) : 1.f);
		}
		// Level, matched to mono. A pair of mics up in the air really is quieter
		// than a pickup sitting on the spot you hit -- measured at 5.3 dB down
		// for a strike dead centre -- but "the stereo switch drops the level"
		// is a wart, not a feature. It is COMMON to both channels, so it sets
		// the level and never the image, and the strike position goes on
		// changing the loudness exactly as much as it does in mono.
		//
		// ENERGY, not the first sample. Matching the transient is one line
		// shorter and it is wrong: the taps redistribute the strike across modes
		// that ring for very different lengths, so an impulse match left the
		// frame drum 7.9 dB loud over a second. The modes are at different
		// frequencies and so very nearly orthogonal in time, which makes the
		// total energy a sum per mode of amplitude squared by decay -- and t60
		// is sitting right there from updateModes().
		float num = 0.f, den = 0.f;
		for (int k = 0; k < NM; k++) {
			float g2 = gain[k] * gain[k], t60 = t60lo[k];
			den += g2 * g2 * t60;
			num += g2 * 0.5f * (tap[0][k] * tap[0][k] + tap[1][k] * tap[1][k]) * t60;
		}
		// Clamped: nothing stops a tap set from coming out near-orthogonal to
		// the strike, and an unbounded 1/num would answer that with a bang.
		float comp = (num > 1e-12f) ? std::sqrt(den / num) : 1.f;
		if (comp < 0.4f) comp = 0.4f;
		if (comp > 2.5f) comp = 2.5f;
		for (int k = 0; k < NM; k++) { tap[0][k] *= comp; tap[1][k] *= comp; }
	}

	// hardness 0..1 spans felt to wood. Contact time follows from mass and
	// stiffness rather than being dialled in: tau ~ pi*sqrt(m/k), so ~5 ms for a
	// soft mallet and ~1 ms for a stick, and it shortens further as the hit gets
	// harder. That is where velocity-to-brightness comes from.
	void strike(float vel, float hardness, float weight) {
		// All three move together, because a soft beater is also a heavy one and
		// the exponent is what actually separates felt from wood. The constants
		// were measured rather than derived: with a Hertzian exponent the units
		// of k are N/m^alpha, so the value that reads like a spring rate is
		// wrong by orders of magnitude and gave 12 ms contacts.
		mallet.expo  = 2.6f - 1.1f * hardness;                    // 2.6 felt, 1.5 wood
		mallet.stiff = 1.0e8f * std::pow(0.1f, hardness);         // 1e8 .. 1e7
		mallet.mass  = weight * (0.05f - 0.035f * hardness);      // heavy felt, light stick
		const float MREF = 0.05f - 0.035f * 0.75f;                // the default EXCITER
		beaterComp = std::pow(MREF / std::max(mallet.mass, 1e-4f), 0.7f);
		mallet.strike(vel, 0.0015f);
		updateMics();          // where this hit is, relative to the two mics

	}

	// One sample, into nch channels. head/snare come back separately so the
	// panel can offer both.
	//
	// Templated on the channel count rather than branched inside the loop: the
	// mono build then compiles to precisely the arithmetic it had before this
	// existed, in the same order, so turning stereo OFF is not a different
	// instrument that happens to be close.
	template <bool ST>
	inline void run(float* headOut, float* snareOut) {
		float dt = 1.f / sr;
		float F = mallet.process(headDisp, dt) * fimp * beaterComp;

		float sum[2] = {0.f, 0.f}, resoSum[2] = {0.f, 0.f}, disp = 0.f, ref = 0.f;
		for (int k = 0; k < NM; k++) {
			if (F != 0.f) { lo[k].hit(F * gain[k]); hi[k].hit(F * gain[k] * 0.6f); }
			float a = lo[k].process(), b = hi[k].process();
			if (ST) {
				float ab = a + b * 0.7f;
				sum[0] += ab * tap[0][k]; resoSum[0] += b * tap[0][k];
				sum[1] += ab * tap[1][k]; resoSum[1] += b * tap[1][k];
				// The strike-point reading, kept even though nothing listens
				// there in stereo. BEND is driven by it, and a pitch envelope
				// that changed shape when you switched the output to stereo
				// would be a bug wearing a feature's clothes.
				ref += ab * gain[k];
			} else {
				sum[0] += (a + b * 0.7f) * gain[k];
				resoSum[0] += b * gain[k];
			}
			disp += a * gain[k];
		}
		headDisp = disp;                    // metres, same unit as the mallet
		energy += (std::fabs(ST ? ref : sum[0]) - energy) * 0.002f;

		// SQUARED, and far quieter. Linear at gain 3 put the wires above the head
		// at the very first notch of the knob -- 1.24x its level at 0.125 -- so
		// there was no such thing as a light dusting of snare. Squared, the
		// bottom of the travel is the whisper it should be and the top still
		// buries the drum if you want it to.
		for (int c = 0; c < (ST ? 2 : 1); c++) {
			snareOut[c] = snareAmt > 0.f
			            ? wires[c].process(resoSum[c] * outGain, snareThr, snareTight, sr)
			              * snareAmt * snareAmt * 0.30f
			            : 0.f;
			headOut[c] = sum[c] * outGain;
		}
		if (ST) airPath(headOut, snareOut);
	}
	// The trip from the head to the two mics. Both signals per channel go
	// through it together -- they left the same drum at the same moment.
	inline void airPath(float* headOut, float* snareOut) {
		dwrite = (dwrite + 1) & (MICDL - 1);
		for (int c = 0; c < 2; c++) {
			dline[c][0][dwrite] = headOut[c];
			dline[c][1][dwrite] = snareOut[c];
		}
		if (micXfade < 1.f) { micXfade += micXrate; if (micXfade > 1.f) micXfade = 1.f; }
		for (int c = 0; c < 2; c++) {
			float g = micLvlOld[c] + (micLvl[c] - micLvlOld[c]) * micXfade;
			for (int j = 0; j < 2; j++) {
				float a = tapDelay(c, j, micDelay[c]);
				if (micXfade < 1.f) {
					float b = tapDelay(c, j, micDelayOld[c]);
					a = b + (a - b) * micXfade;
				}
				(j == 0 ? headOut : snareOut)[c] = a * g;
			}
		}
	}
	inline float tapDelay(int c, int j, float d) const {
		int i = (int)d;
		float f = d - i;
		const float* L = dline[c][j];
		float a = L[(dwrite - i + MICDL) & (MICDL - 1)];
		float b = L[(dwrite - i - 1 + MICDL) & (MICDL - 1)];
		return a + (b - a) * f;
	}
	void process(float* headOut, float* snareOut, bool stereo) {
		if (stereo) run<true>(headOut, snareOut); else run<false>(headOut, snareOut);
	}

	void clear() {
		for (int k = 0; k < NM; k++) { lo[k].clear(); hi[k].clear(); }
		mallet.reset();
		for (int c = 0; c < 2; c++) wires[c].clear();
		std::memset(dline, 0, sizeof dline);
		micXfade = 1.f;
		// The mic delays are in SAMPLES, and clear() is what a sample-rate
		// change calls. Re-solving them here means they are never left over
		// from the old rate waiting for the next strike to correct them.
		updateMics();
		micXfade = 1.f;
		energy = 0.f; headDisp = 0.f;
	}
};

}  // namespace sfs
