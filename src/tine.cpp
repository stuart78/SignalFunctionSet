#include "plugin.hpp"
#include <cmath>


struct Tine : Module {
	enum ParamId {
		FREQ_PARAM,
		DAMPING_PARAM,
		PING_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		IN_INPUT,
		DAMPING_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	// 3rd-order IIR filter state (double precision for stability)
	double xn[4] = {};
	double yn[4] = {};

	// Filter coefficients: current (smoothed) and target
	double b[4] = {};
	double a[4] = {};
	double bTarget[4] = {};
	double aTarget[4] = {};
	bool coeffsInitialized = false;

	// Trigger detection + shaped pulse
	dsp::SchmittTrigger pingTrigger;
	int pulseCounter = 0;
	static const int PULSE_LEN = 16;

	// VCA mode: crossfade envelope on ping
	bool vcaMode = true;
	float vcaEnvelope = 1.f;
	bool vcaFadingOut = false;   // true = fading out before new ping
	bool vcaPendingPing = false; // a ping is waiting for fade-out to complete

	// Smoothed parameter values
	float smoothFreq = 261.63f;
	float smoothMu = 3500.f;
	int coeffUpdateCounter = 0;

	Tine() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configParam(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);
		configParam(DAMPING_PARAM, 0.f, 1.f, 0.5f, "Damping");
		configButton(PING_PARAM, "Ping");

		configInput(VOCT_INPUT, "V/Oct");
		configInput(IN_INPUT, "Trigger (ping)");
		configInput(DAMPING_CV_INPUT, "Damping CV");

		configOutput(OUT_OUTPUT, "Audio");
	}

	void onReset() override {
		for (int i = 0; i < 4; i++) {
			xn[i] = 0.0;
			yn[i] = 0.0;
		}
		coeffsInitialized = false;
	}

	void recalcCoefficients(float freqHz, float mu, float sampleRate) {
		// Gamelan Resonator at X=12 (the standard, stable configuration)
		const double X = 12.0;

		double RC = std::sqrt(3.0) / (2.0 * M_PI * (double)freqHz);
		double RC2 = RC * RC;
		double RC3 = RC2 * RC;

		double mu_d = (double)mu;
		double Kn = mu_d / (mu_d + 1.0);

		double A_coeff = 9.0 / RC;
		double B_coeff = X / RC2;
		double C_coeff = 4.0 * (mu_d + 4.0) / ((mu_d + 1.0) * RC);
		double D_coeff = 3.0 * (mu_d + 13.0) / ((mu_d + 1.0) * RC2);
		double E_coeff = X / RC3;

		// Bilinear transform with frequency pre-warping
		double omega_a = 2.0 * M_PI * (double)freqHz;
		double omega_d = omega_a / (2.0 * (double)sampleRate);
		if (omega_d > 1.5) omega_d = 1.5;
		double c = omega_a / std::tan(omega_d);
		double c2 = c * c;
		double c3 = c2 * c;

		double da0 = c3 + C_coeff * c2 + D_coeff * c + E_coeff;
		double da1 = -3.0 * c3 - C_coeff * c2 + D_coeff * c + 3.0 * E_coeff;
		double da2 = 3.0 * c3 - C_coeff * c2 - D_coeff * c + 3.0 * E_coeff;
		double da3 = -c3 + C_coeff * c2 - D_coeff * c + E_coeff;

		double nb0 = Kn * (c2 + A_coeff * c + B_coeff);
		double nb1 = Kn * (-c2 + A_coeff * c + 3.0 * B_coeff);
		double nb2 = Kn * (-c2 - A_coeff * c + 3.0 * B_coeff);
		double nb3 = Kn * (c2 - A_coeff * c + B_coeff);

		if (std::fabs(da0) < 1e-30) da0 = 1e-30;
		double inv_a0 = 1.0 / da0;
		bTarget[0] = nb0 * inv_a0;
		bTarget[1] = nb1 * inv_a0;
		bTarget[2] = nb2 * inv_a0;
		bTarget[3] = nb3 * inv_a0;
		aTarget[0] = 1.0;
		aTarget[1] = da1 * inv_a0;
		aTarget[2] = da2 * inv_a0;
		aTarget[3] = da3 * inv_a0;

		if (!coeffsInitialized) {
			for (int i = 0; i < 4; i++) {
				b[i] = bTarget[i];
				a[i] = aTarget[i];
			}
			coeffsInitialized = true;
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "vcaMode", json_boolean(vcaMode));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* vcaJ = json_object_get(rootJ, "vcaMode");
		if (vcaJ)
			vcaMode = json_boolean_value(vcaJ);
	}

	void process(const ProcessArgs& args) override {
		// --- Frequency ---
		float pitch = params[FREQ_PARAM].getValue();
		if (inputs[VOCT_INPUT].isConnected())
			pitch += inputs[VOCT_INPUT].getVoltage();
		float freq = dsp::FREQ_C4 * std::pow(2.f, pitch);
		freq = clamp(freq, 8.f, std::min(20000.f, args.sampleRate * 0.45f));

		// --- Damping (µ) ---
		float dampKnob = params[DAMPING_PARAM].getValue();
		if (inputs[DAMPING_CV_INPUT].isConnected())
			dampKnob += inputs[DAMPING_CV_INPUT].getVoltage() / 5.f;
		dampKnob = clamp(dampKnob, 0.f, 1.f);
		// 0→µ=2 (dead thump), 0.5→µ≈50, 0.75→µ≈500, 1.0→µ=50000
		float mu = 2.f * std::pow(50000.f / 2.f, dampKnob);

		// --- Smooth parameters ---
		float smoothAlpha = 1.f - std::exp(-args.sampleTime / 0.005f);
		smoothFreq += (freq - smoothFreq) * smoothAlpha;
		smoothMu += (mu - smoothMu) * smoothAlpha;

		// --- Recalculate target coefficients every 32 samples ---
		if (coeffUpdateCounter++ >= 32) {
			coeffUpdateCounter = 0;
			recalcCoefficients(smoothFreq, smoothMu, args.sampleRate);
		}

		// --- Interpolate coefficients toward target ---
		double coeffAlpha = 1.0 - std::exp(-(double)args.sampleTime / 0.002);
		for (int i = 0; i < 4; i++) {
			b[i] += (bTarget[i] - b[i]) * coeffAlpha;
			a[i] += (aTarget[i] - a[i]) * coeffAlpha;
		}

		// --- Input: trigger only ---
		double input = 0.0;

		// Detect ping requests
		bool newPing = false;
		if (pingTrigger.process(inputs[IN_INPUT].getVoltage(), 0.1f, 1.f))
			newPing = true;
		if (params[PING_PARAM].getValue() > 0.f) {
			params[PING_PARAM].setValue(0.f);
			newPing = true;
		}

		if (newPing) {
			if (vcaMode && vcaEnvelope > 0.01f) {
				// Old ring is still active — fade out first, then ping
				vcaFadingOut = true;
				vcaPendingPing = true;
			} else {
				// No active ring or VCA off — fire immediately
				pulseCounter = PULSE_LEN;
				if (vcaMode) {
					vcaEnvelope = 0.f;
					vcaFadingOut = false;
				}
			}
		}

		// VCA crossfade: fade out old ring, then fire pending ping, then fade in
		if (vcaMode && vcaFadingOut) {
			float rampRate = args.sampleTime / 0.0025f;
			vcaEnvelope -= rampRate;
			if (vcaEnvelope <= 0.f) {
				vcaEnvelope = 0.f;
				vcaFadingOut = false;
				if (vcaPendingPing) {
					// Clear old filter state so new ping starts clean
					for (int i = 0; i < 4; i++) {
						xn[i] = 0.0;
						yn[i] = 0.0;
					}
					pulseCounter = PULSE_LEN;
					vcaPendingPing = false;
				}
			}
		}

		// Shaped pulse: sine arch
		if (pulseCounter > 0) {
			float t = (float)(PULSE_LEN - pulseCounter) / (float)PULSE_LEN;
			input = (double)(std::sin(t * (float)M_PI));
			pulseCounter--;
		}

		// Anti-denormal
		input += 1e-20;

		// --- IIR filter ---
		xn[3] = xn[2]; xn[2] = xn[1]; xn[1] = xn[0];
		yn[3] = yn[2]; yn[2] = yn[1]; yn[1] = yn[0];
		xn[0] = input;

		yn[0] = b[0] * xn[0] + b[1] * xn[1] + b[2] * xn[2] + b[3] * xn[3]
		        - a[1] * yn[1] - a[2] * yn[2] - a[3] * yn[3];

		// Soft clamp in feedback
		if (yn[0] > 10.0) yn[0] = 10.0;
		if (yn[0] < -10.0) yn[0] = -10.0;

		// --- VCA envelope: ramp up after ping (if not currently fading out) ---
		if (vcaMode && !vcaFadingOut && vcaEnvelope < 1.f) {
			float rampRate = args.sampleTime / 0.0025f; // 0→1 in 2.5ms
			vcaEnvelope += rampRate;
			if (vcaEnvelope > 1.f) vcaEnvelope = 1.f;
		}
		float env = vcaMode ? vcaEnvelope : 1.f;

		// --- Output ---
		outputs[OUT_OUTPUT].setVoltage(clamp((float)(yn[0] * 30000.0 * (double)env), -10.f, 10.f));
	}
};


struct TineWidget : ModuleWidget {
	TineWidget(Tine* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/tine.svg")));

		// Screws
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// 8HP = 40.64mm, 3 columns: 10.16, 20.32, 30.48

		// Freq knob (huge, centered) Y=30.48
		addParam(createParamCentered<RoundHugeBlackKnob>(
			mm2px(Vec(20.32f, 30.48f)), module, Tine::FREQ_PARAM));

		// Middle row Y=60.96: Damping knob (small), Damp CV (center), Ping (right)
		addParam(createParamCentered<Trimpot>(
			mm2px(Vec(10.16f, 60.96f)), module, Tine::DAMPING_PARAM));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(20.32f, 60.96f)), module, Tine::DAMPING_CV_INPUT));
		addParam(createParamCentered<VCVButton>(
			mm2px(Vec(30.48f, 60.96f)), module, Tine::PING_PARAM));

		// Bottom row Y=116.84: Trig (left), V/Oct (center), Out (right)
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(10.16f, 116.84f)), module, Tine::IN_INPUT));
		addInput(createInputCentered<PJ301MPort>(
			mm2px(Vec(20.32f, 116.84f)), module, Tine::VOCT_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(
			mm2px(Vec(30.48f, 116.84f)), module, Tine::OUT_OUTPUT));
	}

	void appendContextMenu(Menu* menu) override {
		Tine* module = dynamic_cast<Tine*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("VCA Mode (anti-click)", "",
			&module->vcaMode));
	}
};


Model* modelTine = createModel<Tine, TineWidget>("Tine");
