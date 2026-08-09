#pragma once
#include "plugin.hpp"

// Shared "encoder-safe" gate/trigger pulse width.
//
// Direct ES-9 outputs are full-sample-rate DAC channels, so the default 1 ms
// trigger is tens of samples wide and always lands cleanly. The Expert Sleepers
// Encoders (8CV / 8GT / ES-5) time-multiplex 8 channels down one connection, so
// each channel is effectively resampled to ~2-16 kHz depending on how many are
// active. A 1 ms pulse can be only ~2 frames wide there -- jittered, halved, or
// dropped outright. Widening the pulse to 3-5 ms makes clocks/gates survive that
// path (and gives a companion V/oct channel time to settle before the gate is
// sampled).
//
// The index is stored (not the raw float) so patches stay stable and the menu
// can render a radio selection. Default index 0 == 1 ms, i.e. unchanged from
// legacy behaviour for existing patches that never touch the setting.
// A DUTY CYCLE is a different thing from a width, and both are wanted.
//
// A width in milliseconds is what a trigger needs: it is about surviving the
// path to the next module, and it should not change when the tempo does. A duty
// cycle is what a GATE needs: half of a quarter note is a musical length, and
// it has to grow and shrink with the clock. Asking for "50%" and getting 5ms
// would be useless, and asking for "encoder-safe" and getting something that
// collapses at 300 BPM would be worse.
//
// So the two live in one list and one stored index -- indices 0-3 stay exactly
// the four fixed widths that shipped, so no saved patch moves -- and a caller
// that knows the period of the output it is driving can offer the duty entries
// as well. A caller that does not know its period simply does not offer them.
namespace sfs {

static const int   NUM_FIXED_WIDTHS = 4;    // indices 0..3 -- absolute milliseconds
static const int   NUM_PULSE_WIDTHS = 9;    // indices 4..8 -- fractions of the period
static const float PULSE_WIDTHS[NUM_PULSE_WIDTHS] = {
	0.001f, 0.002f, 0.005f, 0.010f,   0.f, 0.f, 0.f, 0.f, 0.f };
static const float PULSE_DUTIES[NUM_PULSE_WIDTHS] = {
	0.f, 0.f, 0.f, 0.f,   0.125f, 0.25f, 0.5f, 0.75f, 0.875f };
static const char* PULSE_WIDTH_LABELS[NUM_PULSE_WIDTHS] = {
	"1 ms (default)", "2 ms", "5 ms (encoder-safe)", "10 ms",
	"12.5% gate", "25% gate", "50% gate", "75% gate", "87.5% gate" };

inline bool pulseIsDuty(int idx) {
	return idx >= NUM_FIXED_WIDTHS && idx < NUM_PULSE_WIDTHS;
}

// Clamp a stored index and return the pulse width in seconds. A duty-cycle
// index reaching a caller with no period to scale against falls back to the
// 1 ms default rather than to silence.
inline float pulseWidthSec(int idx) {
	if (idx < 0 || idx >= NUM_FIXED_WIDTHS) idx = 0;
	return PULSE_WIDTHS[idx];
}

// The same, for an output whose own period is known. `periodSec` must be the
// interval to the NEXT pulse on that output, not the one just finished: under
// swing those differ by up to 3x, and scaling the wrong one lets a 75% gate run
// past the pulse that should end it -- which reads downstream as a gate that
// never falls.
inline float pulseWidthSec(int idx, float periodSec) {
	if (pulseIsDuty(idx) && periodSec > 0.f)
		return periodSec * PULSE_DUTIES[idx];
	return pulseWidthSec(idx);
}

// Append a "Gate/trigger width" submenu that reads/writes *idxPtr. The current
// choice shows as the submenu's right-hand text.
// `allowDuty` is for modules whose outputs have a known period -- a clock. The
// duty entries are separated by a divider because they answer a different
// question from the widths above them.
inline void addPulseWidthMenu(Menu* menu, int* idxPtr,
                              const char* label = "Gate/trigger width",
                              bool allowDuty = false) {
	int last = allowDuty ? NUM_PULSE_WIDTHS : NUM_FIXED_WIDTHS;
	int cur = (*idxPtr >= 0 && *idxPtr < last) ? *idxPtr : 0;
	menu->addChild(createSubmenuItem(label, PULSE_WIDTH_LABELS[cur],
		[idxPtr, last](Menu* sub) {
			for (int i = 0; i < last; i++) {
				if (i == NUM_FIXED_WIDTHS)
					sub->addChild(createMenuLabel("Gate, as a share of the step"));
				sub->addChild(createCheckMenuItem(PULSE_WIDTH_LABELS[i], "",
					[idxPtr, i]() { return *idxPtr == i; },
					[idxPtr, i]() { *idxPtr = i; }));
			}
		}));
}

} // namespace sfs
