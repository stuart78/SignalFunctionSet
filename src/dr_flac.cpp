// dr_flac implementation translation unit (Play loads .flac multisamples).
// Kept in its own file so play.cpp — which includes the header for declarations
// only — doesn't pay dr_flac's compile cost on every edit. Mirrors how dr_wav's
// implementation lives in phase.cpp.
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
