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

  // SFS: a copy of the lowest carrier op's amplitude envelope (for display).
  Env carrierEnv() const;

 private:
  FmCore core_;
  Env env_[6];
  FmOpParams params_[6];
  PitchEnv pitchenv_;
  int32_t basepitch_[6];
  int32_t fb_buf_[2];
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
