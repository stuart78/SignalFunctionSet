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

	void updateBrownianMotion(int outputIndex, float phase) {
		// Update Brownian motion when phase wraps (once per cycle)
		if (phase < lastBrownianPhase[outputIndex]) {
			// Phase wrapped, take a Brownian step
			float step = 2.f * (random::uniform() - 0.5f) * 0.3f; // Random step ±0.3
			brownianValue[outputIndex] += step;
			// Keep bounded to ±1 range with soft limiting
			brownianValue[outputIndex] = clamp(brownianValue[outputIndex], -2.f, 2.f);
			if (std::abs(brownianValue[outputIndex]) > 1.f) {
				brownianValue[outputIndex] *= 0.8f; // Soft pull back toward center
			}
		}
		lastBrownianPhase[outputIndex] = phase;
	}

	float generateWave(float phase, float shape, int outputIndex) {
		// Morph between sine (0), triangle (0.25), saw (0.5), square (0.75), noise (1)
		if (shape < 0.25f) {
			// Sine to Triangle
			float mix = shape * 4.f;
			float sine = std::sin(phase * 2.f * M_PI);
			float tri = 2.f * std::abs(std::fmod(phase + 0.25f, 1.f) - 0.5f) - 1.f;
			return sine * (1.f - mix) + tri * mix;
		}
		else if (shape < 0.5f) {
			// Triangle to Saw
			float mix = (shape - 0.25f) * 4.f;
			float tri = 2.f * std::abs(std::fmod(phase + 0.25f, 1.f) - 0.5f) - 1.f;
			float saw = 2.f * std::fmod(phase, 1.f) - 1.f;
			return tri * (1.f - mix) + saw * mix;
		}
		else if (shape < 0.75f) {
			// Saw to Square
			float mix = (shape - 0.5f) * 4.f;
			float saw = 2.f * std::fmod(phase, 1.f) - 1.f;
			float square = (std::fmod(phase, 1.f) < 0.5f) ? 1.f : -1.f;
			return saw * (1.f - mix) + square * mix;
		}
		else {
			// Square to Smooth Random (Brownian Motion)
			float mix = (shape - 0.75f) * 4.f;
			float square = (std::fmod(phase, 1.f) < 0.5f) ? 1.f : -1.f;
			
			// Update Brownian motion for this output
			updateBrownianMotion(outputIndex, phase);
			
			// Interpolate between square and smooth random
			return square * (1.f - mix) + brownianValue[outputIndex] * mix;
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

		// Update phases
		float deltaPhase = actualFreq * args.sampleTime;
		for (int i = 0; i < 4; i++) {
			phases[i] += deltaPhase;
			if (phases[i] >= 1.f)
				phases[i] -= 1.f;
		}

		// Update Lorenz attractors for each output
		float lorenzDt = args.sampleTime * actualFreq * (1.f - stability) * 0.5f; // Scale with frequency and instability
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
			float adjustedPhase = phases[i] + phaseOffsets[i];
			if (adjustedPhase >= 1.f)
				adjustedPhase -= 1.f;

			// Generate base wave
			float wave = generateWave(adjustedPhase, shape, i);
			
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
				wave = generateWave(freqModulatedPhase, shape, i) * ampMod;
				
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

		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(12.648, 12.6)), module, QuadLFO::PARAMSHAPE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(37.875, 12.6)), module, QuadLFO::PARAMSTABILITY_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(12.648, 36.235)), module, QuadLFO::PARAMFREQUENCY_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(37.875, 36.235)), module, QuadLFO::PARAMXSPREAD_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(12.648, 59.624)), module, QuadLFO::PARAMCENTER_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(37.875, 59.624)), module, QuadLFO::PARAMYSPREAD_PARAM));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(6.306, 77.395)), module, QuadLFO::INSHAPE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.955, 77.395)), module, QuadLFO::INSTABILITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.568, 77.395)), module, QuadLFO::INFREQUENCY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.182, 77.395)), module, QuadLFO::INSPREAD_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(18.955, 93.128)), module, QuadLFO::INCENTER_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(31.568, 93.128)), module, QuadLFO::INYSPREAD_INPUT));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(6.306, 93.128)), module, QuadLFO::OUTMIN_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.182, 93.128)), module, QuadLFO::OUTMAX_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(6.306, 109.458)), module, QuadLFO::OUTA_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(18.955, 109.458)), module, QuadLFO::OUTB_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(31.568, 109.458)), module, QuadLFO::OUTC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.182, 109.458)), module, QuadLFO::OUTD_OUTPUT));
	}
};


Model* modelQuadLFO = createModel<QuadLFO, QuadLFOWidget>("QuadLFO");