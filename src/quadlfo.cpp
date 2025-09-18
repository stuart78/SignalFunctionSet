#include "plugin.hpp"


struct QuadLFO : Module {
	enum ParamId {
		PARAMSHAPE_PARAM,
		PARAMSTABILITY_PARAM,
		PARAMFREQUENCY_PARAM,
		PARAMXSPREAD_PARAM,
		PARAMCENTER_PARAM,
		PARAMYSPREAD_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		INSHAPE_INPUT,
		INSTABILITY_INPUT,
		INFREQUENCY_INPUT,
		INSPREAD_INPUT,
		INCENTER_INPUT,
		INYSPREAD_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTMIN_OUTPUT,
		OUTMAX_OUTPUT,
		OUTA_OUTPUT,
		OUTB_OUTPUT,
		OUTC_OUTPUT,
		OUTD_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	float phases[4] = {0.f, 0.f, 0.f, 0.f};
	float baseFreq = 1.0f;
	dsp::SchmittTrigger clockTrigger;
	float clockFreq = 1.0f;
	int clockSampleCount = 0;
	int lastClockSample = 0;
	bool clockConnected = false;
	
	// Lorenz attractor state for each output (x, y, z coordinates)
	struct LorenzState {
		float x, y, z;
		LorenzState() : x(1.f), y(1.f), z(1.f) {}
	};
	LorenzState lorenz[4];
	
	// Lorenz parameters - classic values with slight variations per output
	float lorenzSigma[4] = {10.0f, 10.2f, 9.8f, 10.1f};
	float lorenzRho[4] = {28.0f, 28.3f, 27.7f, 28.1f};
	float lorenzBeta[4] = {2.667f, 2.7f, 2.6f, 2.65f};
	
	// Brownian motion state for smooth random waveform
	float brownianValue[4] = {0.f, 0.f, 0.f, 0.f};
	float brownianTarget[4] = {0.f, 0.f, 0.f, 0.f};
	float lastBrownianPhase[4] = {0.f, 0.f, 0.f, 0.f};

	QuadLFO() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(PARAMSHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape", "", 0.f, 1.f);
		configParam(PARAMSTABILITY_PARAM, 0.f, 1.f, 0.5f, "Stability", "", 0.f, 1.f);
		configParam(PARAMFREQUENCY_PARAM, -4.9f, 3.32f, 0.f, "Frequency", " Hz", 2.f, 1.f);
		configParam(PARAMXSPREAD_PARAM, 0.f, 1.f, 0.f, "X Spread", "", 0.f, 1.f);
		configParam(PARAMCENTER_PARAM, -5.f, 5.f, 0.f, "Center", " V");
		configParam(PARAMYSPREAD_PARAM, 0.f, 5.f, 5.f, "Y Spread", " V");
		
		configInput(INSHAPE_INPUT, "Shape CV");
		configInput(INSTABILITY_INPUT, "Stability CV");
		configInput(INFREQUENCY_INPUT, "Clock");
		configInput(INSPREAD_INPUT, "X Spread CV");
		configInput(INCENTER_INPUT, "Center CV");
		configInput(INYSPREAD_INPUT, "Y Spread CV");
		
		configOutput(OUTMIN_OUTPUT, "Min");
		configOutput(OUTMAX_OUTPUT, "Max");
		configOutput(OUTA_OUTPUT, "Output A");
		configOutput(OUTB_OUTPUT, "Output B");
		configOutput(OUTC_OUTPUT, "Output C");
		configOutput(OUTD_OUTPUT, "Output D");
	}

	void updateBrownianMotion(int outputIndex, float phase, float sampleTime) {
		// Update Brownian motion when phase wraps (once per cycle)
		if (phase < lastBrownianPhase[outputIndex]) {
			// Phase wrapped, take a small Brownian step toward a new target
			float step = 2.f * (random::uniform() - 0.5f) * 0.1f; // Much smaller steps ±0.1
			brownianTarget[outputIndex] += step;
			
			// Keep target bounded with gentle pull toward center
			if (std::abs(brownianTarget[outputIndex]) > 0.8f) {
				brownianTarget[outputIndex] *= 0.9f; // Gentle pull back toward center
			}
			brownianTarget[outputIndex] = clamp(brownianTarget[outputIndex], -1.2f, 1.2f);
		}
		
		// Smooth interpolation toward target (creates inertia and smoothness)
		float smoothing = 0.02f; // Very slow movement for smoothness
		brownianValue[outputIndex] += (brownianTarget[outputIndex] - brownianValue[outputIndex]) * smoothing;
		
		lastBrownianPhase[outputIndex] = phase;
	}

	float generateWave(float phase, float shape, int outputIndex, float sampleTime) {
		// Generate base waveforms - all bipolar ±1, perfectly phase-aligned
		// All waveforms start at 0 when phase=0
		
		float sine = std::sin(phase * 2.f * M_PI);
		
		// Triangle wave: starts at 0, goes to +1 at 0.25, back to 0 at 0.5, to -1 at 0.75, back to 0 at 1.0
		float triangle;
		if (phase < 0.25f) {
			triangle = 4.f * phase; // 0 to +1
		} else if (phase < 0.75f) {
			triangle = 2.f - 4.f * phase; // +1 to -1
		} else {
			triangle = 4.f * phase - 4.f; // -1 to 0
		}
		
		// Sawtooth wave: starts at 0, ramps linearly to +1 at 0.5, jumps to -1, ramps to 0 at 1.0
		float sawtooth;
		if (phase < 0.5f) {
			sawtooth = 2.f * phase; // 0 to +1
		} else {
			sawtooth = 2.f * phase - 2.f; // -1 to 0 (after jump from +1 to -1)
		}
		
		// Square wave: special handling to start at 0 and maintain symmetry
		// Use a sine-based approach for smooth transitions that starts at 0
		float square = (std::sin(phase * 2.f * M_PI) >= 0.f) ? 1.f : -1.f;
		// Override the exact phase=0 case to ensure it starts at 0
		if (phase < 0.001f || phase > 0.999f) {
			square = 0.f;
		}
		
		// Brownian motion chaos
		updateBrownianMotion(outputIndex, phase, sampleTime);
		float chaos = brownianValue[outputIndex];
		
		// Morphing between waveforms using smooth interpolation
		if (shape <= 0.25f) {
			// Sine to Triangle (0.0 to 0.25)
			float mix = shape * 4.f;
			return sine * (1.f - mix) + triangle * mix;
		}
		else if (shape <= 0.5f) {
			// Triangle to Sawtooth (0.25 to 0.5)
			float mix = (shape - 0.25f) * 4.f;
			return triangle * (1.f - mix) + sawtooth * mix;
		}
		else if (shape <= 0.75f) {
			// Sawtooth to Square (0.5 to 0.75)
			float mix = (shape - 0.5f) * 4.f;
			return sawtooth * (1.f - mix) + square * mix;
		}
		else {
			// Square to Chaos (0.75 to 1.0)
			float mix = (shape - 0.75f) * 4.f;
			return square * (1.f - mix) + chaos * mix;
		}
	}

	void process(const ProcessArgs& args) override {
		// Get parameters with CV
		float shape = params[PARAMSHAPE_PARAM].getValue();
		if (inputs[INSHAPE_INPUT].isConnected())
			shape += inputs[INSHAPE_INPUT].getVoltage() / 10.f;
		shape = clamp(shape, 0.f, 1.f);

		float stability = params[PARAMSTABILITY_PARAM].getValue();
		if (inputs[INSTABILITY_INPUT].isConnected())
			stability += inputs[INSTABILITY_INPUT].getVoltage() / 10.f;
		stability = clamp(stability, 0.f, 1.f);

		float freq = params[PARAMFREQUENCY_PARAM].getValue();
		float actualFreq = std::pow(2.f, freq);
		
		// Handle clock input
		clockConnected = inputs[INFREQUENCY_INPUT].isConnected();
		if (clockConnected) {
			if (clockTrigger.process(inputs[INFREQUENCY_INPUT].getVoltage())) {
				// Rising edge detected - reset phase A and calculate frequency from clock
				if (lastClockSample > 0) {
					int samplesBetweenClocks = clockSampleCount - lastClockSample;
					if (samplesBetweenClocks > 0) {
						float clockPeriod = samplesBetweenClocks * args.sampleTime;
						clockFreq = 1.f / clockPeriod;
						// Clamp to reasonable range
						clockFreq = clamp(clockFreq, 0.1f, 100.f);
					}
				}
				lastClockSample = clockSampleCount;
				phases[0] = 0.f; // Reset output A phase on each clock
			}
			clockSampleCount++;
			actualFreq = clockFreq;
		}

		float xSpread = params[PARAMXSPREAD_PARAM].getValue();
		if (inputs[INSPREAD_INPUT].isConnected())
			xSpread += inputs[INSPREAD_INPUT].getVoltage() / 10.f;
		xSpread = clamp(xSpread, 0.f, 1.f);

		float center = params[PARAMCENTER_PARAM].getValue();
		if (inputs[INCENTER_INPUT].isConnected())
			center += inputs[INCENTER_INPUT].getVoltage();
		center = clamp(center, -5.f, 5.f);

		float ySpread = params[PARAMYSPREAD_PARAM].getValue();
		if (inputs[INYSPREAD_INPUT].isConnected())
			ySpread += inputs[INYSPREAD_INPUT].getVoltage();
		ySpread = clamp(ySpread, 0.f, 5.f);

		// Calculate phase spreads for the 4 outputs
		float phaseOffsets[4] = {0.f, 0.25f, 0.5f, 0.75f};
		
		// Apply x spread to phase offsets
		for (int i = 0; i < 4; i++) {
			phaseOffsets[i] *= xSpread;
		}

		// Update master phase only
		float deltaPhase = actualFreq * args.sampleTime;
		phases[0] += deltaPhase;
		if (phases[0] >= 1.f)
			phases[0] -= 1.f;

		// Update Lorenz attractors for each output
		float lorenzDt = args.sampleTime * (1.f - stability) * 2.0f; // Scale with instability only, not frequency
		for (int i = 0; i < 4; i++) {
			if (stability < 1.f) { // Only update when instability is present
				// Lorenz equations: dx/dt = σ(y-x), dy/dt = x(ρ-z)-y, dz/dt = xy-βz
				float dx = lorenzSigma[i] * (lorenz[i].y - lorenz[i].x);
				float dy = lorenz[i].x * (lorenzRho[i] - lorenz[i].z) - lorenz[i].y;
				float dz = lorenz[i].x * lorenz[i].y - lorenzBeta[i] * lorenz[i].z;
				
				// Integrate using Euler method
				lorenz[i].x += dx * lorenzDt;
				lorenz[i].y += dy * lorenzDt;
				lorenz[i].z += dz * lorenzDt;
			}
		}

		// Generate outputs
		float outputs[4];
		float minVal = 10.f, maxVal = -10.f;

		for (int i = 0; i < 4; i++) {
			float adjustedPhase = phases[0] + phaseOffsets[i];
			if (adjustedPhase >= 1.f)
				adjustedPhase -= 1.f;

			// Generate base wave
			float wave = generateWave(adjustedPhase, shape, i, args.sampleTime);
			
			// Apply Lorenz attractor-based stability modulation
			if (stability < 1.f) {
				float instabilityAmount = 1.f - stability;
				
				// Normalize Lorenz coordinates to usable ranges
				// X and Y typically range ±20, Z ranges 0-50
				float lorenzX = clamp(lorenz[i].x / 20.f, -1.f, 1.f);   // Phase modulation
				float lorenzY = clamp(lorenz[i].y / 20.f, -1.f, 1.f);   // Amplitude modulation  
				float lorenzZ = clamp((lorenz[i].z - 25.f) / 25.f, -1.f, 1.f); // Frequency modulation
				
				// Phase modulation - creates "drift" in timing
				float phaseOffset = lorenzX * instabilityAmount * 0.1f;
				float driftedPhase = adjustedPhase + phaseOffset;
				while (driftedPhase < 0.f) driftedPhase += 1.f;
				while (driftedPhase >= 1.f) driftedPhase -= 1.f;
				
				// Amplitude modulation - creates "breathing" effect
				float ampMod = 1.f + (lorenzY * instabilityAmount * 0.3f);
				ampMod = clamp(ampMod, 0.3f, 1.7f); // Reasonable range
				
				// Frequency modulation - creates subtle tempo variations
				float freqMod = 1.f + (lorenzZ * instabilityAmount * 0.05f);
				float freqModulatedPhase = driftedPhase * freqMod;
				while (freqModulatedPhase < 0.f) freqModulatedPhase += 1.f;
				while (freqModulatedPhase >= 1.f) freqModulatedPhase -= 1.f;
				
				// Generate the wave with all modulations
				wave = generateWave(freqModulatedPhase, shape, i, args.sampleTime) * ampMod;
				
				// Add subtle harmonic content based on Lorenz Z
				float harmonicContent = std::sin(freqModulatedPhase * 3.f * M_PI) * lorenzZ * instabilityAmount * 0.1f;
				wave += harmonicContent;
			}

			// Scale and offset
			outputs[i] = center + wave * ySpread;
			
			// Track min/max
			minVal = std::min(minVal, outputs[i]);
			maxVal = std::max(maxVal, outputs[i]);
		}

		// Set outputs
		this->outputs[OUTA_OUTPUT].setVoltage(outputs[0]);
		this->outputs[OUTB_OUTPUT].setVoltage(outputs[1]);
		this->outputs[OUTC_OUTPUT].setVoltage(outputs[2]);
		this->outputs[OUTD_OUTPUT].setVoltage(outputs[3]);
		this->outputs[OUTMIN_OUTPUT].setVoltage(minVal);
		this->outputs[OUTMAX_OUTPUT].setVoltage(maxVal);
	}
};


struct QuadLFOWidget : ModuleWidget {
	QuadLFOWidget(QuadLFO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/quadlfo.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.1, 15.0)), module, QuadLFO::PARAMSTABILITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.7, 15.0)), module, QuadLFO::PARAMSHAPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.1, 62.5)), module, QuadLFO::PARAMXSPREAD_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.7, 37.5)), module, QuadLFO::PARAMCENTER_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.7, 62.5)), module, QuadLFO::PARAMFREQUENCY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(38.1, 37.5)), module, QuadLFO::PARAMYSPREAD_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.306, 85.365)), module, QuadLFO::INSHAPE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.955, 85.365)), module, QuadLFO::INSTABILITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.568, 85.365)), module, QuadLFO::INFREQUENCY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.182, 85.365)), module, QuadLFO::INSPREAD_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.955, 102.033)), module, QuadLFO::INCENTER_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.568, 102.033)), module, QuadLFO::INYSPREAD_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.182, 102.033)), module, QuadLFO::OUTMAX_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(6.255, 102.454)), module, QuadLFO::OUTMIN_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(6.306, 117.363)), module, QuadLFO::OUTA_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(18.955, 117.363)), module, QuadLFO::OUTB_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(31.568, 117.363)), module, QuadLFO::OUTC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.182, 117.363)), module, QuadLFO::OUTD_OUTPUT));
	}
};


Model* modelQuadLFO = createModel<QuadLFO, QuadLFOWidget>("QuadLFO");
