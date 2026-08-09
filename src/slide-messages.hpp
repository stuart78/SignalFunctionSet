#pragma once
// Slide → Slide X expander bus.
//
// Slide (the mother) writes this into its rightExpander.producerMessage every
// sample and flips; the expander reads it from
// leftExpander.module->rightExpander.consumerMessage. Same direction and same
// cadence as Meter → Meter X, which is the one this plugin has proven; the
// other way round is the one that bit OP MORPH.
//
// Audio through an expander message costs one sample of latency, which is
// inaudible and is the price of not making Slide 8HP wider.
struct SlideExpanderMessage {
	static const int NCH = 8;
	float string[NCH] = {};   // per-string audio, already soft-clipped, in volts
	bool  active = false;     // the mother is really there and rendering
};
