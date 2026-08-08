#pragma once
#include <cstdint>

// Rack-free wrapper around the vendored msfa DX7 engine. Kept in its own
// translation unit so the msfa headers (which define global `Module`, `min`,
// `max`, `N`) never collide with Rack's `using namespace rack;`.
//
// Block-based: the engine renders BLOCK (64) samples at a time. The host reads
// control state once per block, calls renderBlock(), then streams the 64
// samples out per channel.

struct BellEngineImpl;

// The DX7 routing of one algorithm, as plain floats: w[src][dst] for dst 0..5
// is how much of operator src is added to operator dst's PHASE (the FM index),
// and w[src][6] is how much of it reaches the output (a plain VCA). Feedback is
// a delayed edge and sits outside the matrix; -1 means none.
//
// Exported in this form so an expander can build and blend routings without
// including any msfa header -- those define global Module/min/max/N and cannot
// meet Rack's `using namespace rack`.
void bellAlgorithmWeights(int algorithm, float w[6][7], int* fbSrc, int* fbDst);

class BellEngine {
public:
	static const int BLOCK = 64;
	static const int MAX_CH = 16;

	BellEngine();
	~BellEngine();

	void setSampleRate(double sr);              // (re)inits lookup tables + LFO

	// Load a DX7 bank: accepts a 4104-byte bulk SysEx (F0 43 00 09 20 00 …) or
	// a raw 4096-byte bank (32 packed voices). Returns true if accepted.
	bool loadBank(const uint8_t* data, int len);
	void getBank(uint8_t out[4096]) const;      // for patch persistence
	void setBankRaw(const uint8_t data[4096]);  // restore from persistence

	void setVoice(int idx);                     // 0..31; reloads patch + LFO
	int  voice() const;
	int  algorithm() const;                     // 0..31 of the current voice
	int  carrierMask() const;                   // bit op set if msfa op is a carrier
	void getVoiceName(char out[11]) const;      // 10-char DX7 name + NUL

	void setPitchBend14(int v);

	// SFS: morph the operator ROUTING toward another algorithm. amount 0 leaves
	// every voice on the untouched engine path; algoB is 0..31.
	void setMorph(int algoB, float amount);

	// Drive the routing directly, overriding setMorph. Passing nullptr hands
	// control back to the algorithm/morph path.
	void setMatrixWeights(const float w[6][7], int fbSrc, int fbDst);
	void clearMatrix();
                 // 0..16383, 8192 = center

	void noteOn(int ch, int midinote, int velocity);
	void noteOff(int ch);

	// Live macros.
	void setPitchOffset(int ch, int32_t off);   // logfreq units, 1 oct = 1<<24
	void setBrightness(float b);                 // -1..+1
	void setFeedbackOffset(int v);               // added to patch feedback
	void setOpEnabled(int op, bool en);          // operator on/off
	static int32_t octavesToLogfreq(float oct);  // helper: octaves -> logfreq

	// Compute one BLOCK for channels 0..nChannels-1 (advances the shared LFO).
	void renderBlock(int nChannels);
	// Normalized sample (~[-1,1]) from the most recent renderBlock().
	float sample(int ch, int i) const;

	// VCO out: a continuous, envelope-bypassed raw tone per channel, independent
	// of gate, for an external ADSR/VCA to shape. setVcoNote (re)arms a channel's
	// oscillator at a midinote (re-armed automatically when the note or the patch
	// changes); setVcoPitchOffset tracks fractional V/oct. Call renderVcoBlock
	// AFTER renderBlock each block (it reuses that block's LFO).
	void setVcoNote(int ch, int midinote);
	void setVcoPitchOffset(int ch, int32_t off);
	void renderVcoBlock(int nChannels);
	float vcoSample(int ch, int i) const;

	// Fill out[0..n-1] with the current voice's carrier amplitude envelope as a
	// normalized 0..1 shape (attack/decay/sustain then release), resampled to n.
	// For the on-screen envelope display; call when the voice changes.
	void renderEnvShape(float* out, int n) const;

private:
	BellEngineImpl* p_;
	BellEngine(const BellEngine&) = delete;
	BellEngine& operator=(const BellEngine&) = delete;
};
