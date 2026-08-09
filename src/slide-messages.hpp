#pragma once
// Slide ↔ Slide X expander bus. Two directions down the same pair of modules,
// and each side OWNS the buffers it writes, so the two never touch:
//
//   Slide  writes its rightExpander.producerMessage and flips it;
//          Slide X reads leftExpander.module->rightExpander.consumerMessage.
//   SlideX writes its leftExpander.producerMessage and flips it;
//          Slide reads rightExpander.module->leftExpander.consumerMessage.
//
// That is the direction and the cadence Meter → Meter X proved, mirrored. The
// other arrangement — one module writing into the other's buffer — is what left
// OP MORPH silent, and it is silent rather than broken, which is worse.
//
// Everything crosses one sample late. Inaudible, and it is the price of not
// making Slide 8HP wider to carry the jacks itself.

// Slide → Slide X: what each string is sounding.
struct SlideExpanderMessage {
	static const int NCH = 8;
	float string[NCH] = {};   // per-string audio, already soft-clipped, in volts
	bool  active = false;     // the mother is really there and rendering
};

// Slide X → Slide: play these strings.
//
// Gates travel as RAW VOLTS rather than as booleans, because the mother already
// owns a SchmittTrigger per string and edge detection belongs wherever the
// triggers live — split it across the bus and the two ends can disagree about
// what counts as an edge.
struct SlideXMessage {
	static const int NCH = 8;
	bool  active = false;
	float gate[NCH] = {};     // volts
	float vel[NCH] = {};      // 0..1, already resolved from the VEL jack
};
