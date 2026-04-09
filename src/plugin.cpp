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

	// Add modules here
	// p->addModel(modelMyModule);

	// Any other plugin initialization may go here.
	// As an alternative, consider lazy-loading assets and lookup tables when your module is created to reduce startup times of Rack.
}
