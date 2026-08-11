/*
 * Copyright 2012 Google Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *      http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SYNTH_DX7NOTE_H_
#define SYNTH_DX7NOTE_H_

// This is the logic to put together a note from the MIDI description
// and run the low-level modules.

// It will continue to evolve a bit, as note-stealing logic, scaling,
// and real-time control of parameters live here.

#include "env.h"
#include "pitchenv.h"
#include "fm_core.h"
#include "fm_matrix.h"

class Dx7Note {
 public:
  // SFS: retrigger keeps the oscillator phases, the operator gains, the
  // feedback history and the envelope levels running, so restarting a voice
  // that is still sounding is continuous. A fresh note still resets them all.
  void init(const char patch[128], int midinote, int velocity,
            bool retrigger = false);

  // Note: this _adds_ to the buffer. Interesting question whether it's
  // worth it...
  // envBypass (SFS): hold every operator at its peak output level instead of
  // running its amplitude envelope — yields a continuous "raw VCO" tone for an
  // external ADSR/VCA to shape. Uses the note's own pitch env + LFO as usual.
  void compute(int32_t *buf, int32_t lfo_val, int32_t lfo_delay,
    const Controllers *ctrls, bool envBypass = false);

  void keyup();

  // SFS addition: live per-note pitch offset (continuous V/oct + tune), in
  // logfreq units (1 octave = 1<<24). Set each block by the host.
  void setPitchOffset(int32_t off) { pitchOffset_ = off; }

  // SFS: morph the ROUTING toward another algorithm. t is Q15; 0 leaves this
  // note on FmCore untouched, so an unmorphed voice is bit-identical.
  void setMorph(int algoB, int32_t t) {
    if (algoB != morphAlgo_ || t != morphT_) { morphAlgo_ = algoB; morphT_ = t; morphDirty_ = true; }
  }
  int  algorithm() const { return algorithm_; }

  // An explicit routing, from an expander. nullptr returns to the algorithm /
  // morph path, and with neither set the note runs stock FmCore untouched.
  void setMatrix(const FmMatrix* m) {
    if (!m) { haveExt_ = false; return; }
    mtxExt_ = *m; haveExt_ = true;
    // Which operators are CARRIERS now comes from the matrix's output column,
    // not from the patch's algorithm. Otherwise the patch's own routing keeps
    // leaking in through everything downstream that asks "is this a carrier":
    // the BRIGHTNESS macro (which lifts modulators only), the VCO's envBypass
    // hold, and the envelope the display draws. Changing the voice would then
    // audibly alter a routing the voice no longer decides.
    for (int op = 0; op < 6; op++) extCarrier_[op] = mtxExt_.w[op][6] > 0;
  }
  bool carrierAt(int op) const { return haveExt_ ? extCarrier_[op] : isCarrier_[op]; }

  // SFS: a copy of the lowest carrier op's amplitude envelope (for display).
  Env carrierEnv() const;

 private:
  FmCore core_;
  FmMatrixCore mcore_;
  FmMatrix     mtx_;
  int     morphAlgo_ = -1;
  int32_t morphT_ = 0;
  bool    morphDirty_ = true;
  FmMatrix mtxExt_;
  bool     haveExt_ = false;
  bool     extCarrier_[6] = {false, false, false, false, false, false};
  Env env_[6];
  FmOpParams params_[6];
  PitchEnv pitchenv_;
  int32_t basepitch_[6] = {};
  int32_t fb_buf_[2] = {};
  int32_t fb_shift_;

  int algorithm_;
  int pitchmoddepth_;
  int pitchmodsens_;

  // SFS: per-op peak level (env "fully open"), used by compute(envBypass=true).
  int32_t opPeakLevel_[6] = {0, 0, 0, 0, 0, 0};

  // SFS additions
  int32_t pitchOffset_ = 0;     // continuous pitch / tune
  int patchFeedback_ = 0;       // patch's feedback value (for the offset macro)
  bool isCarrier_[6] = {false, false, false, false, false, false};
};

#endif  // SYNTH_DX7NOTE_H_
