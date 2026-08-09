#pragma once
// Meter → Meter X expander bus. Meter (mother) writes this into its
// rightExpander.producerMessage each sample and flips; the expander reads it
// from leftExpander.module->rightExpander.consumerMessage.
struct MeterExpanderMessage {
	bool running = false;     // clock is running (drives the RUN gate out)
	bool ppqn24 = false;      // a 24-PPQN clock pulse fired this sample
	bool bar[8] = {};         // 1, 2, 4, 8, 16, 32, 64, 128-bar pulse fired this sample
	float barPos = 0.f;       // continuous position in bars since reset (for cycle pie charts)
	// How long a bar and a quarter currently are, in seconds. The expander needs
	// these to turn a duty-cycle setting ("50% gate") into a pulse length: only
	// the mother knows the tempo, and a gate that is a share of the step has to
	// follow it. Zero until the mother has run a sample, which reads as "no
	// period known" and falls the expander back to a fixed-width trigger.
	float barSec = 0.f, quarterSec = 0.f;
};
