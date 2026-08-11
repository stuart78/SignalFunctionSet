// Rack-free implementation of OpEnvEngine over the vendored msfa DX7 envelope.
#include "op_env_engine.h"

#include <cstring>
#include <algorithm>
#include <cmath>

#include "msfa/synth.h"
#include "msfa/exp2.h"
#include "msfa/sin.h"
#include "msfa/env.h"
#include "msfa/lfo.h"
#include "msfa/patch.h"
#include "msfa/fm_core.h"

#include "bell_patches.h"

// Fixed "full output" reference so the envelope reaches its peak; the CV is then
// normalized to the configured envelope's own peak (see lmax_).
static const int OUTLEVEL = 4064;   // scaleoutlevel(99) << 5

struct OpEnvEngine::Impl {
	uint8_t bank[4096];
	char    unpacked[156] = {};
	int     curVoice = 0;
	double  sr = 0.0;
	bool    tablesInit = false;

	Env   env;
	bool  keyed = false;
	int   rateOff[4] = {0, 0, 0, 0};
	int   levelOff[4] = {0, 0, 0, 0};
	int   rates[4] = {0, 0, 0, 0};      // voice carrier EG + offsets, clamped
	int   levels[4] = {0, 0, 0, 0};
	int   carrierOp = 0;
	int32_t lmax = 1;                   // level that maps to full scale (1.0)
	float curLevel = 0.f;
	float scratch[6144] = {};

	// LFO / tremolo
	Lfo   lfo;
	int   lfoRateOff = 0, lfoDelayOff = 0, lfoDepthOff = 0, amSensOff = 0;
	int   waveOverride = -1;
	float amDepth = 0.f;                // 0..1 tremolo depth (AMD x AM sens)

	// rate scaling (V/oct)
	int   note = 60;
	bool  voctConnected = false;

	float releaseFrac = 0.7f;          // where key-off lands in the rendered shape

	// As an envelope generator, gate-off should return to 0V. The DX7 release
	// stage ramps to L4 and holds there, so a voice (or L4 offset) with L4 > 0
	// would leave the output stuck high. When true, release targets silence
	// instead; false keeps the authentic DX7 L4 release level.
	bool  releaseToZero = true;

	Impl() {
		bellpatch::buildDefaultBank(bank);
		std::memset(unpacked, 0, sizeof(unpacked));
		UnpackPatch((const char*)(bank), unpacked);
	}
};

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// DX7 key rate scaling (replicated from dx7note.cc).
static inline int scaleRate(int midinote, int sens) {
	int x = std::min(31, std::max(0, midinote / 3 - 7));
	return (sens * x) >> 3;
}

OpEnvEngine::OpEnvEngine() : p_(new Impl) {}
OpEnvEngine::~OpEnvEngine() { delete p_; }

// Recompute the carrier EG (voice + offsets), full-scale reference, and LFO.
static void updateParams(OpEnvEngine::Impl* p) {
	int alg = (uint8_t) p->unpacked[134];
	int cm = fmCoreCarrierMask(alg);
	int op = 0;
	for (int i = 0; i < 6; i++) if ((cm >> i) & 1) { op = i; break; }
	p->carrierOp = op;
	int off = op * 21;
	for (int i = 0; i < 4; i++) {
		p->rates[i]  = clampi((uint8_t) p->unpacked[off + i]     + p->rateOff[i],  0, 99);
		p->levels[i] = clampi((uint8_t) p->unpacked[off + 4 + i] + p->levelOff[i], 0, 99);
	}
	int maxL = std::max(std::max(p->levels[0], p->levels[1]), std::max(p->levels[2], p->levels[3]));
	int al = Env::scaleoutlevel(maxL) >> 1;
	al = (al << 6) + OUTLEVEL - 4256;
	if (al < 16) al = 16;
	p->lmax = al << 16;

	// LFO: rate/delay/wave from voice (137..142) with offsets/override.
	char lp[6];
	lp[0] = (char) clampi((uint8_t) p->unpacked[137] + p->lfoRateOff,  0, 99);   // speed
	lp[1] = (char) clampi((uint8_t) p->unpacked[138] + p->lfoDelayOff, 0, 99);   // delay
	lp[2] = p->unpacked[139];                                                    // pmd (unused)
	lp[3] = p->unpacked[140];                                                    // amd (unused here)
	lp[4] = p->unpacked[141];                                                    // sync
	lp[5] = (char)(p->waveOverride >= 0 ? p->waveOverride : (uint8_t) p->unpacked[142]);
	p->lfo.reset(lp);

	// Tremolo depth = AMD x carrier AM sensitivity (both with offsets).
	int amd = clampi((uint8_t) p->unpacked[140] + p->lfoDepthOff, 0, 99);
	int amsens = clampi((uint8_t) p->unpacked[off + 14] + p->amSensOff, 0, 3);
	p->amDepth = (amd / 99.f) * (amsens / 3.f);
}

void OpEnvEngine::setSampleRate(double sr) {
	if (sr <= 0.0) return;
	p_->sr = sr;
	Exp2::init();
	Sin::init();
	Lfo::init(sr);
	Env::init_sr(sr);
	p_->tablesInit = true;
	updateParams(p_);
	// Idle at rest: arm the envelope then release it to silence so it sits at
	// the floor. Always release to 0 here (regardless of releaseToZero) — a
	// voice with L4 > 0 would otherwise ramp up to L4 on launch with no gate.
	p_->env.init(p_->rates, p_->levels, OUTLEVEL, 0);
	p_->env.setparam(7, 0);
	p_->env.keydown(false);
	p_->keyed = false;
}

void OpEnvEngine::setVoice(int idx) {
	idx = clampi(idx, 0, 31);
	p_->curVoice = idx;
	UnpackPatch((const char*)(p_->bank + 128 * idx), p_->unpacked);
	updateParams(p_);
}

int OpEnvEngine::voice() const { return p_->curVoice; }

void OpEnvEngine::getVoiceName(char out[11]) const {
	std::memcpy(out, p_->unpacked + 145, 10);
	out[10] = '\0';
	for (int i = 0; i < 10; i++)
		if (out[i] < 32 || out[i] > 126) out[i] = ' ';
}

bool OpEnvEngine::loadBank(const uint8_t* data, int len) {
	if (!data) return false;
	if (len >= 4104 && data[0] == 0xF0 && data[1] == 0x43 && data[3] == 0x09 && data[4] == 0x20)
		std::memcpy(p_->bank, data + 6, 4096);
	else if (len >= 4096)
		std::memcpy(p_->bank, data, 4096);
	else
		return false;
	setVoice(p_->curVoice);
	return true;
}

void OpEnvEngine::getBank(uint8_t out[4096]) const { std::memcpy(out, p_->bank, 4096); }
void OpEnvEngine::setBankRaw(const uint8_t data[4096]) {
	std::memcpy(p_->bank, data, 4096);
	setVoice(p_->curVoice);
}

void OpEnvEngine::setOffsets(const int rateOff[4], const int levelOff[4]) {
	for (int i = 0; i < 4; i++) { p_->rateOff[i] = rateOff[i]; p_->levelOff[i] = levelOff[i]; }
	updateParams(p_);
}

void OpEnvEngine::setLfo(int rateOff, int delayOff, int depthOff, int amSensOff, int waveOverride) {
	p_->lfoRateOff = rateOff; p_->lfoDelayOff = delayOff; p_->lfoDepthOff = depthOff;
	p_->amSensOff = amSensOff; p_->waveOverride = waveOverride;
	updateParams(p_);
}

void OpEnvEngine::setNote(int midinote, bool connected) {
	p_->note = clampi(midinote, 0, 127);
	p_->voctConnected = connected;
}

void OpEnvEngine::gate(bool down) {
	if (!p_->tablesInit) return;
	if (down) {
		int rs = p_->voctConnected
			? scaleRate(p_->note, (uint8_t) p_->unpacked[p_->carrierOp * 21 + 13]) : 0;
		p_->env.init(p_->rates, p_->levels, OUTLEVEL, rs);
		p_->lfo.keydown();           // reset LFO delay ramp (tremolo fades in)
		p_->keyed = true;
	} else {
		// Release toward silence (env-generator behavior) by overriding L4 for
		// this release; env.init on the next gate restores the voice's real L4.
		if (p_->releaseToZero) p_->env.setparam(7, 0);
		p_->env.keydown(false);
		p_->keyed = false;
	}
}

void OpEnvEngine::setReleaseToZero(bool v) { p_->releaseToZero = v; }

void OpEnvEngine::renderBlock() {
	if (!p_->tablesInit) return;
	int32_t lv = p_->env.getsample();
	float a = std::exp2((float)(lv - p_->lmax) / (float)(1 << 24));
	if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f;
	// Tremolo: the LFO dips amplitude by amDepth, faded in via the LFO delay.
	if (p_->amDepth > 0.f) {
		float lfo01 = (float) p_->lfo.getsample() / (float)(1 << 24);
		float dly01 = (float) p_->lfo.getdelay() / (float)(1 << 24);
		float trem = 1.f - p_->amDepth * dly01 * lfo01;
		a *= trem;
	} else {
		p_->lfo.getsample();         // keep phase advancing
		p_->lfo.getdelay();
	}
	p_->curLevel = a;
}

float OpEnvEngine::level() const { return p_->curLevel; }

void OpEnvEngine::renderEnvShape(float* out, int n) const {
	if (!out || n <= 0) return;
	for (int i = 0; i < n; i++) out[i] = 0.f;
	if (!p_->tablesInit) return;

	Env e;
	e.init(p_->rates, p_->levels, OUTLEVEL, 0);
	float* buf = p_->scratch;
	const int CAP = 3000, MAXREC = 6000;
	int cnt = 0; int32_t prev = -2147483647; int stable = 0;
	for (int k = 0; k < CAP && cnt < MAXREC; k++) {
		int32_t lv = e.getsample();
		buf[cnt++] = (float) lv;
		if (lv == prev) { if (++stable > 24) break; } else stable = 0;
		prev = lv;
	}
	int relIdx = cnt;                 // buf index where release (key-off) begins
	if (p_->releaseToZero) e.setparam(7, 0);   // mirror gate() release-to-silence
	e.keydown(false);
	prev = -2147483647; stable = 0;
	for (int k = 0; k < CAP && cnt < MAXREC; k++) {
		int32_t lv = e.getsample();
		buf[cnt++] = (float) lv;
		if (lv == prev) { if (++stable > 24) break; } else stable = 0;
		prev = lv;
	}
	if (cnt < 2) return;

	float peak = buf[0];
	for (int i = 1; i < cnt; i++) if (buf[i] > peak) peak = buf[i];
	const float inv = 1.0f / (float)(1 << 24);
	for (int i = 0; i < cnt; i++) buf[i] = std::exp2((buf[i] - peak) * inv);

	if (cnt + 1 < 6144) {
		for (int i = cnt; i > 0; i--) buf[i] = buf[i - 1];
		buf[0] = 0.f;
		cnt++;
		relIdx++;
	}
	int peakIdx = 0; float pk = buf[0];
	for (int i = 1; i < cnt; i++) if (buf[i] > pk) { pk = buf[i]; peakIdx = i; }
	float attackFrac = std::max(0.15f, (float) peakIdx / (float)(cnt - 1));
	int attackPts = (int)(attackFrac * (n - 1));
	if (attackPts < 1) attackPts = 1;
	if (attackPts > n - 2) attackPts = n - 2;
	for (int i = 0; i < n; i++) {
		float t;
		if (i <= attackPts)
			t = (float) peakIdx * (float) i / (float) attackPts;
		else
			t = peakIdx + (float)(cnt - 1 - peakIdx) * (float)(i - attackPts) / (float)(n - 1 - attackPts);
		int i0 = (int) t; if (i0 > cnt - 1) i0 = cnt - 1;
		int i1 = i0 + 1;  if (i1 > cnt - 1) i1 = cnt - 1;
		float fr = t - (float) i0;
		out[i] = buf[i0] * (1.f - fr) + buf[i1] * fr;
	}

	// Where does key-off land along the rendered curve?
	float rf;
	if (relIdx <= peakIdx)        rf = (float) attackPts / (float)(n - 1);
	else if (cnt - 1 <= peakIdx)  rf = 1.f;
	else {
		float bi = attackPts + (float)(n - 1 - attackPts)
			* (float)(relIdx - peakIdx) / (float)(cnt - 1 - peakIdx);
		rf = bi / (float)(n - 1);
	}
	p_->releaseFrac = rf < 0.f ? 0.f : (rf > 1.f ? 1.f : rf);
}

static float levelToAmp(int level, int32_t lmax) {
	int al = Env::scaleoutlevel(level) >> 1;
	al = (al << 6) + OUTLEVEL - 4256;
	if (al < 16) al = 16;
	int32_t Lint = (int32_t) al << 16;
	float a = std::exp2((float)(Lint - lmax) / (float)(1 << 24));
	return a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
}

void OpEnvEngine::getLevelAmps(float out[4]) const {
	for (int i = 0; i < 4; i++) out[i] = levelToAmp(p_->levels[i], p_->lmax);
}

float OpEnvEngine::releaseFraction() const { return p_->releaseFrac; }
