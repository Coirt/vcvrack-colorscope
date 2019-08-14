#include "plugin.hpp"


Plugin *plugin;

void init(rack::Plugin *p) {
	p->addModel(modelScope);
}
