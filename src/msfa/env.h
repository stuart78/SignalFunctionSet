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

#ifndef __ENV_H
#define __ENV_H

// DX7 envelope generation

class Env {
 public:

  // The rates and levels arrays are calibrated to match the Dx7 parameters
  // (ie, value 0..99). The outlevel parameter is calibrated in microsteps
  // (ie units of approx .023 dB), with 99 * 32 = nominal full scale. The
  // rate_scaling parameter is in qRate units (ie 0..63).
  // SFS: keepLevel re-enters the attack from the CURRENT level instead of from
  // silence. Zeroing it is right for a fresh note and wrong for a retrigger --
  // a channel that is still sounding drops to zero in one sample, which is a
  // full-amplitude step, and a step is spectrally flat, so it thumps.
  void init(const int rates[4], const int levels[4], int outlevel,
      int rate_scaling, bool keepLevel = false);

  // Result is in Q24/doubling log format. Also, result is subsampled
  // for every N samples.
  // A couple more things need to happen for this to be used as a gain
  // value. First, the # of outputs scaling needs to be applied. Also,
  // modulation.
  // Then, of course, log to linear.
  int32_t getsample();

  void keydown(bool down);
  void setparam(int param, int value);
  static int scaleoutlevel(int outlevel);

  // SFS: the original msfa rate constants are calibrated for a fixed output
  // rate (44.1k). init_sr scales the per-step increment so envelope *times* are
  // correct at any host sample rate (port of Dexed's fix). Call once at setup.
  static void init_sr(double sampleRate);
 private:
  // Default-initialised for the same reason Lfo's members are: an Env that is
  // constructed but not yet init()'d otherwise holds whatever was on the heap,
  // and a static analyser is right to say so (issue #11). init() overwrites all
  // of these before the first getsample(), so this costs nothing but certainty.
  int rates_[4] = {0, 0, 0, 0};
  int levels_[4] = {0, 0, 0, 0};
  int outlevel_ = 0;
  int rate_scaling_ = 0;
  // Level is stored so that 2^24 is one doubling, ie 16 more bits than
  // the DX7 itself (fraction is stored in level rather than separate
  // counter)
  int32_t level_ = 0;
  int targetlevel_ = 0;
  bool rising_ = false;
  int ix_ = 0;
  int inc_ = 0;

  bool down_ = true;

  void advance(int newix);
};

#endif  // __ENV_H

