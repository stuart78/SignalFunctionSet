#pragma once
// OpMorph → Operator expander bus.
//
// Operator (the mother) sits on the LEFT; OpMorph writes a complete routing
// into its own leftExpander.producerMessage each sample and flips, and Operator
// reads it from rightExpander.consumerMessage. Deliberately plain floats: the
// matrix itself lives behind msfa headers, whose global Module/min/max/N cannot
// meet Rack's `using namespace rack`, so nothing here refers to them.
struct OpMorphMessage {
	bool  active = false;      // false hands Operator back to its own algorithm
	float w[6][7] = {};        // [src][dst]; cols 0..5 are FM depth, col 6 output
	int   fbSrc = -1, fbDst = -1;
};
