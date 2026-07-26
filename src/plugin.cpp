#include "plugin.hpp"


Plugin* pluginInstance;


void init(Plugin* p) {
	pluginInstance = p;
	p->addModel(modelDrift);
	p->addModel(modelGsx);
	p->addModel(modelFugue);
	p->addModel(modelPhase);
	p->addModel(modelOvertone);
	p->addModel(modelIntone);
	p->addModel(modelTine);
	p->addModel(modelMeter);
	p->addModel(modelMeterExpander);
	p->addModel(modelFugueX);
	p->addModel(modelMetaFugue);
	p->addModel(modelBeat);
	p->addModel(modelNote);
	p->addModel(modelSwell);
	p->addModel(modelShift);
	p->addModel(modelWave);
	p->addModel(modelVac);
	p->addModel(modelMuse);
	p->addModel(modelGravity);
	p->addModel(modelBand);
	p->addModel(modelCycle);
	p->addModel(modelOperator);
	p->addModel(modelOpEnv);
	p->addModel(modelChance);
	p->addModel(modelRecord);
	p->addModel(modelPlay);
	p->addModel(modelRatio);
	p->addModel(modelArrange);
	p->addModel(modelFill);
	p->addModel(modelChime);
	p->addModel(modelCrystal);

	// Add modules here
	// p->addModel(modelMyModule);

	// Any other plugin initialization may go here.
	// As an alternative, consider lazy-loading assets and lookup tables when your module is created to reduce startup times of Rack.
}
