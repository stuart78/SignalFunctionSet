// Rack-free implementation of BellEngine over the vendored msfa engine.
// This TU includes the msfa headers; nothing Rack is included here, so the
// msfa global symbols (Module/min/max/N) stay contained.
#include "bell_voice.h"

#include <cstring>
#include <algorithm>
#include <cmath>

#include "msfa/synth.h"
#include "msfa/freqlut.h"
#include "msfa/sin.h"
#include "msfa/exp2.h"
#include "msfa/pitchenv.h"
#include "msfa/lfo.h"
#include "msfa/patch.h"
#include "msfa/controllers.h"
#include "msfa/dx7note.h"

#include "bell_patches.h"

struct BellEngineImpl {
	uint8_t bank[4096];            // 32 packed voices
	char    unpacked[156];         // current voice, unpacked
	int     curVoice = 0;
	double  sr = 0.0;
	bool    tablesInit = false;

	Lfo lfo;
	Controllers controllers;
	Dx7Note notes[BellEngine::MAX_CH];
	bool  active[BellEngine::MAX_CH];   // channel is rendering (note still audible)
	bool  keyed[BellEngine::MAX_CH];    // note currently held (between on and off)
	int   quietBlocks[BellEngine::MAX_CH];  // consecutive near-silent blocks after release
	float vbuf[BellEngine::MAX_CH][BellEngine::BLOCK];

	// VCO out: continuous env-bypassed oscillators, independent of gate.
	Dx7Note vco[BellEngine::MAX_CH];
	bool    vcoArmed[BellEngine::MAX_CH];   // vco[ch] init()'d with current patch
	int     vcoNote[BellEngine::MAX_CH];    // midinote it was armed at
	float   vcoBuf[BellEngine::MAX_CH][BellEngine::BLOCK];
	int32_t lfoVal = 0, lfoDelay = 0;       // last block's LFO (shared with VCO)
	float   envScratch[6144];               // for renderEnvShape (off the stack)

	BellEngineImpl() {
		// Default bank: the four Brian Eno DX7 patches (Keyboard, Feb 1987),
		// repeated to fill all 32 slots.
		bellpatch::buildDefaultBank(bank);
		std::memset(vbuf, 0, sizeof(vbuf));
		std::memset(vcoBuf, 0, sizeof(vcoBuf));
		for (int i = 0; i < BellEngine::MAX_CH; i++) {
			active[i] = false;
			keyed[i] = false;
			quietBlocks[i] = 0;
			vcoArmed[i] = false;
			vcoNote[i] = -1;
		}
		controllers.values_[kControllerPitch] = 0x2000;  // center
	}
};

BellEngine::BellEngine() : p_(new BellEngineImpl) {}
BellEngine::~BellEngine() { delete p_; }

void BellEngine::setSampleRate(double sr) {
	if (sr <= 0.0) return;
	p_->sr = sr;
	Freqlut::init(sr);
	Exp2::init();
	Sin::init();
	Lfo::init(sr);
	PitchEnv::init(sr);
	Env::init_sr(sr);   // SFS: keep envelope times correct at the host SR
	p_->tablesInit = true;
	// Reload the current voice so the LFO picks up the new rate.
	setVoice(p_->curVoice);
}

void BellEngine::setVoice(int idx) {
	if (idx < 0) idx = 0;
	if (idx > 31) idx = 31;
	p_->curVoice = idx;
	UnpackPatch((const char*)(p_->bank + 128 * idx), p_->unpacked);
	if (p_->tablesInit)
		p_->lfo.reset((const char*)(p_->unpacked + 137));
	// Patch changed: VCO oscillators must re-arm with the new patch.
	for (int i = 0; i < MAX_CH; i++) p_->vcoArmed[i] = false;
}

int BellEngine::voice() const { return p_->curVoice; }
int BellEngine::algorithm() const { return (uint8_t) p_->unpacked[134]; }
int BellEngine::carrierMask() const { return fmCoreCarrierMask((uint8_t) p_->unpacked[134]); }

void BellEngine::getVoiceName(char out[11]) const {
	std::memcpy(out, p_->unpacked + 145, 10);
	out[10] = '\0';
	// Replace non-printable chars with spaces.
	for (int i = 0; i < 10; i++)
		if (out[i] < 32 || out[i] > 126) out[i] = ' ';
}

bool BellEngine::loadBank(const uint8_t* data, int len) {
	if (!data) return false;
	if (len >= 4104 && data[0] == 0xF0 && data[1] == 0x43 &&
	    data[3] == 0x09 && data[4] == 0x20) {
		// 32-voice bulk dump: F0 43 0n 09 20 00 <4096> chk F7
		std::memcpy(p_->bank, data + 6, 4096);
	} else if (len >= 4096) {
		// Raw 4096-byte bank.
		std::memcpy(p_->bank, data, 4096);
	} else {
		return false;
	}
	setVoice(p_->curVoice);
	return true;
}

void BellEngine::getBank(uint8_t out[4096]) const { std::memcpy(out, p_->bank, 4096); }
void BellEngine::setBankRaw(const uint8_t data[4096]) {
	std::memcpy(p_->bank, data, 4096);
	setVoice(p_->curVoice);
}

void BellEngine::setPitchBend14(int v) {
	if (v < 0) v = 0; if (v > 16383) v = 16383;
	p_->controllers.values_[kControllerPitch] = v;
}

void BellEngine::noteOn(int ch, int midinote, int velocity) {
	if (ch < 0 || ch >= MAX_CH || !p_->tablesInit) return;
	// A channel that is still sounding is RE-triggered rather than rebuilt: a
	// fresh init zeroes every operator phase and every envelope level, so the
	// output falls to zero in one sample while the previous note is still
	// audible. That step is what clicks -- and being a step it is spectrally
	// flat, so on a low patch it reads as a thump. A real DX7 never hits this,
	// because consecutive notes land on different voices and the old one is left
	// to release; here VCV's poly convention reuses the channel.
	p_->notes[ch].init(p_->unpacked, midinote, velocity, p_->active[ch]);
	p_->active[ch] = true;
	p_->keyed[ch] = true;
	p_->quietBlocks[ch] = 0;
	p_->lfo.keydown();
}

void BellEngine::noteOff(int ch) {
	if (ch < 0 || ch >= MAX_CH) return;
	p_->notes[ch].keyup();
	p_->keyed[ch] = false;
}

void BellEngine::setPitchOffset(int ch, int32_t off) {
	if (ch < 0 || ch >= MAX_CH) return;
	p_->notes[ch].setPitchOffset(off);
}
void BellEngine::setBrightness(float b) { p_->controllers.brightness = b; }
void BellEngine::setFeedbackOffset(int v) { p_->controllers.feedbackOffset = v; }
void BellEngine::setOpEnabled(int op, bool en) {
	if (op >= 0 && op < 6) p_->controllers.opEnabled[op] = en;
}
int32_t BellEngine::octavesToLogfreq(float oct) {
	return (int32_t)(oct * (float)(1 << 24));
}

void BellEngine::renderBlock(int nChannels) {
	if (!p_->tablesInit) return;
	if (nChannels < 0) nChannels = 0;
	if (nChannels > MAX_CH) nChannels = MAX_CH;
	int32_t lfoval = p_->lfo.getsample();
	int32_t lfodelay = p_->lfo.getdelay();
	p_->lfoVal = lfoval;       // shared with the VCO render (don't advance twice)
	p_->lfoDelay = lfodelay;
	const float norm = 1.0f / (float)(1 << 28);
	for (int ch = 0; ch < nChannels; ch++) {
		// Only compute channels that have actually been triggered — an
		// untriggered Dx7Note has an uninitialized algorithm_, which would index
		// FmCore's algorithm table out of bounds (crash).
		if (!p_->active[ch]) {
			std::memset(p_->vbuf[ch], 0, sizeof(p_->vbuf[ch]));
			continue;
		}
		int32_t buf[BLOCK];
		std::memset(buf, 0, sizeof(buf));
		p_->notes[ch].compute(buf, lfoval, lfodelay, &p_->controllers);
		float peak = 0.f;
		for (int i = 0; i < BLOCK; i++) {
			float s = buf[i] * norm;
			p_->vbuf[ch][i] = s;
			float a = std::fabs(s);
			if (a > peak) peak = a;
		}
		// Free a released channel once it has decayed to silence. The msfa
		// envelope floors at a tiny non-zero level, so without this every played
		// note would drone faintly forever (16 of them = a noisy wash).
		if (!p_->keyed[ch] && peak < 3.0e-4f) {
			if (++p_->quietBlocks[ch] >= 8) {
				p_->active[ch] = false;
				std::memset(p_->vbuf[ch], 0, sizeof(p_->vbuf[ch]));
			}
		} else {
			p_->quietBlocks[ch] = 0;
		}
	}
}

float BellEngine::sample(int ch, int i) const {
	if (ch < 0 || ch >= MAX_CH || i < 0 || i >= BLOCK) return 0.f;
	return p_->vbuf[ch][i];
}

// ---- VCO out: continuous, env-bypassed raw tone (for an external ADSR) ------

void BellEngine::setVcoNote(int ch, int midinote) {
	if (ch < 0 || ch >= MAX_CH || !p_->tablesInit) return;
	if (midinote < 0) midinote = 0; if (midinote > 127) midinote = 127;
	if (!p_->vcoArmed[ch] || p_->vcoNote[ch] != midinote) {
		// Retrigger once armed, for the same reason the note path does -- a plain
		// init zeroes every operator phase, gain, feedback tail and envelope
		// level, so the output drops to zero for a sample. Here it matters MORE:
		// this voice is re-inited every time V/OCT crosses a semitone, not only
		// when a key is struck, and it is supposed to be a continuous oscillator
		// for an external ADSR to shape. Under envBypass the carriers hold at
		// peak but the MODULATORS run their envelopes, so a reset also collapses
		// the timbre to a bare carrier and blooms it back at every semitone.
		p_->vco[ch].init(p_->unpacked, midinote, 127, p_->vcoArmed[ch]);
		p_->vcoNote[ch] = midinote;
		p_->vcoArmed[ch] = true;
	}
}

void BellEngine::setVcoPitchOffset(int ch, int32_t off) {
	if (ch < 0 || ch >= MAX_CH) return;
	if (p_->vcoArmed[ch]) p_->vco[ch].setPitchOffset(off);
}

void BellEngine::renderVcoBlock(int nChannels) {
	if (!p_->tablesInit) return;
	if (nChannels < 0) nChannels = 0;
	if (nChannels > MAX_CH) nChannels = MAX_CH;
	const float norm = 1.0f / (float)(1 << 28);
	for (int ch = 0; ch < nChannels; ch++) {
		if (!p_->vcoArmed[ch]) { std::memset(p_->vcoBuf[ch], 0, sizeof(p_->vcoBuf[ch])); continue; }
		int32_t buf[BLOCK];
		std::memset(buf, 0, sizeof(buf));
		p_->vco[ch].compute(buf, p_->lfoVal, p_->lfoDelay, &p_->controllers, /*envBypass=*/true);
		for (int i = 0; i < BLOCK; i++)
			p_->vcoBuf[ch][i] = buf[i] * norm;
	}
}

float BellEngine::vcoSample(int ch, int i) const {
	if (ch < 0 || ch >= MAX_CH || i < 0 || i >= BLOCK) return 0.f;
	return p_->vcoBuf[ch][i];
}

void BellEngine::renderEnvShape(float* out, int n) const {
	if (!out || n <= 0) return;
	for (int i = 0; i < n; i++) out[i] = 0.f;
	if (!p_->tablesInit) return;

	// Simulate the lowest carrier's amplitude envelope on a copy (note C4, full
	// velocity): attack/decay until it settles, then release until it bottoms.
	Dx7Note tmp;
	tmp.init(p_->unpacked, 60, 100);
	Env e = tmp.carrierEnv();

	float* buf = p_->envScratch;
	const int CAP = 3000;        // per phase (~4 s @48k before clipping)
	const int MAXREC = 6000;
	int cnt = 0; int32_t prev = -2147483647; int stable = 0;
	for (int k = 0; k < CAP && cnt < MAXREC; k++) {
		int32_t lv = e.getsample();
		buf[cnt++] = (float) lv;
		if (lv == prev) { if (++stable > 24) break; } else stable = 0;
		prev = lv;
	}
	e.keydown(false);           // release
	prev = -2147483647; stable = 0;
	for (int k = 0; k < CAP && cnt < MAXREC; k++) {
		int32_t lv = e.getsample();
		buf[cnt++] = (float) lv;
		if (lv == prev) { if (++stable > 24) break; } else stable = 0;
		prev = lv;
	}
	if (cnt < 2) return;

	// Level is log2 (Q24 doubling); convert to linear amplitude relative to peak.
	float peak = buf[0];
	for (int i = 1; i < cnt; i++) if (buf[i] > peak) peak = buf[i];
	const float inv = 1.0f / (float)(1 << 24);
	for (int i = 0; i < cnt; i++)
		buf[i] = std::exp2((buf[i] - peak) * inv);   // 0..1

	// Prepend a silence sample so the curve starts at 0 and the attack reads as a
	// rise. getsample()'s first call already jumps through a fast attack, so the
	// pre-peak samples aren't recorded — without this, fast-attack patches start
	// already at the top.
	if (cnt + 1 < 6144) {
		for (int i = cnt; i > 0; i--) buf[i] = buf[i - 1];
		buf[0] = 0.f;
		cnt++;
	}

	// Peak-aware resample: give the attack (silence → peak) at least 15% of the
	// width so it's always a visible line up, even when it's near-instant.
	int peakIdx = 0; float pk = buf[0];
	for (int i = 1; i < cnt; i++) if (buf[i] > pk) { pk = buf[i]; peakIdx = i; }
	float attackFrac = std::max(0.15f, (float) peakIdx / (float)(cnt - 1));
	int attackPts = (int)(attackFrac * (n - 1));
	if (attackPts < 1) attackPts = 1;
	if (attackPts > n - 2) attackPts = n - 2;
	for (int i = 0; i < n; i++) {
		float t;
		if (i <= attackPts)
			t = (float) peakIdx * (float) i / (float) attackPts;          // silence → peak
		else
			t = peakIdx + (float)(cnt - 1 - peakIdx) * (float)(i - attackPts)
			              / (float)(n - 1 - attackPts);                    // peak → release
		int i0 = (int) t; if (i0 > cnt - 1) i0 = cnt - 1;
		int i1 = i0 + 1;  if (i1 > cnt - 1) i1 = cnt - 1;
		float fr = t - (float) i0;
		out[i] = buf[i0] * (1.f - fr) + buf[i1] * fr;
	}
}
