#include <string.h>
/*
#ifdef PQ_VERSION
#include "PQ.hpp"
#include "Common/Branding.hpp"
#define FONT "res/fonts/LessPerfectDOSVGA.ttf"
#define LETTER_SPACING -1.25
#define FONT_SIZE 12
#else
*/
#include "plugin.hpp"
#define FONT "res/fonts/Sudo.ttf"
#define LETTER_SPACING -2
#define FONT_SIZE 13
//#endif

static const int BUFFER_SIZE = 512;

struct Scope : Module {
	enum ParamIds {
		X_SCALE_PARAM,
		X_POS_PARAM,
		Y_SCALE_PARAM,
		Y_POS_PARAM,
		TIME_PARAM,
		LISSAJOUS_PARAM,
		TRIG_PARAM,
		EXTERNAL_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		X_INPUT,
		Y_INPUT,
		TRIG_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		NUM_OUTPUTS
	};
	enum LightIds {
		PLOT_LIGHT,
		LISSAJOUS_LIGHT,
		INTERNAL_LIGHT,
		EXTERNAL_LIGHT,
		NUM_LIGHTS
	};

	float bufferX[BUFFER_SIZE] = {};
	float bufferY[BUFFER_SIZE] = {};
	int bufferIndex = 0;
	float frameIndex = 0;

	dsp::BooleanTrigger sumTrigger;
	dsp::BooleanTrigger extTrigger;
	bool lissajous = false;
	bool external = false;
	dsp::SchmittTrigger resetTrigger;
	PortWidget *xPort, *yPort;

	Scope() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(X_SCALE_PARAM, -2.0f, 8.0f, 0.0f);
		configParam(X_POS_PARAM, -10.0f, 10.0f, 0.0f);
		configParam(Y_SCALE_PARAM, -2.0f, 8.0f, 0.0f);
		configParam(Y_POS_PARAM, -10.0f, 10.0f, 0.0f);
		configParam(TIME_PARAM, -6.0f, -16.0f, -14.0f);
		configParam(LISSAJOUS_PARAM, 0.0f, 1.0f, 0.0f);
		configParam(TRIG_PARAM, -10.0f, 10.0f, 0.0f);
		configParam(EXTERNAL_PARAM, 0.0f, 1.0f, 0.0f);

	}

	void process(const ProcessArgs &args) override {
		// Modes
		if (sumTrigger.process(params[LISSAJOUS_PARAM].getValue() > 0.f)) {
			lissajous = !lissajous;
		}
		lights[PLOT_LIGHT].setBrightness(lissajous ? 0.f : 1.f);//perhaps not !
		lights[LISSAJOUS_LIGHT].setBrightness(lissajous ? 1.f : 0.f);

		if (extTrigger.process(params[EXTERNAL_PARAM].value)) {
			external = !external;
		}
		lights[INTERNAL_LIGHT].setBrightness(!external ? 0.f : 1.f);//perhaps not !
		lights[EXTERNAL_LIGHT].setBrightness(external ? 1.f : 0.f);

		// Compute time
		float deltaTime = std::pow(2.f, -params[TIME_PARAM].value);//perhaps not inverse
		int frameCount = (int) std::ceil(deltaTime * args.sampleRate);

		// Add frame to buffer
		if (bufferIndex < BUFFER_SIZE) {
			if (++frameIndex > frameCount) {
				frameIndex = 0;
				bufferX[bufferIndex] = inputs[X_INPUT].getVoltage();
				bufferY[bufferIndex] = inputs[Y_INPUT].getVoltage();
				bufferIndex++;
			}
		}

		// Are we waiting on the next trigger?
		if (bufferIndex >= BUFFER_SIZE) {
			// Trigger immediately if external but nothing plugged in, or in Lissajous mode
			if (lissajous || (external && !inputs[TRIG_INPUT].isConnected())) {
				bufferIndex = 0;
				frameIndex = 0;
				return;
			}

			// Reset the Schmitt trigger so we don't trigger immediately if the input is high
			if (frameIndex == 0) {
				resetTrigger.reset();
			}
			frameIndex++;

			// Must go below 0.1fV to trigger
			float gate = external ? inputs[TRIG_INPUT].getVoltage() : inputs[X_INPUT].getVoltage();

			// Reset if triggered
			float holdTime = 0.1f;
			if (resetTrigger.process(rescale(gate, params[TRIG_PARAM].getValue() - 0.1f, params[TRIG_PARAM].getValue(), 0.f, 1.f)) || (frameIndex >= args.sampleTime * holdTime)) {
				bufferIndex = 0; frameIndex = 0; return;
			}

			// Reset if we've waited too long
			if (frameIndex >= args.sampleTime * holdTime) {
				bufferIndex = 0; frameIndex = 0; return;
			}
		}
	}

	json_t *dataToJson() override {
		json_t *rootJ = json_object();
		json_object_set_new(rootJ, "lissajous", json_integer((int) lissajous));
		json_object_set_new(rootJ, "external", json_integer((int) external));
		return rootJ;
	}

	void dataFromJson(json_t *rootJ) override {
		json_t *sumJ = json_object_get(rootJ, "lissajous");
		if (sumJ)
			lissajous = json_integer_value(sumJ);

		json_t *extJ = json_object_get(rootJ, "external");
		if (extJ)
			external = json_integer_value(extJ);
	}

	void onReset() override {
		lissajous = false;
		external = false;
	}
};

struct ScopeDisplay : TransparentWidget {
	Scope *module;
	int frame = 0;
	std::shared_ptr<Font> font;

	struct Stats {
		float vrms = 0.f, vpp = 0.f, vmin = 0.f, vmax = 0.f;

		void calculate(float *values) {
			vrms = 0.f;
			vmax = -INFINITY;
			vmin = INFINITY;
			for (int i = 0; i < BUFFER_SIZE; i++) {
				float v = values[i];
				vrms += v*v;
				vmax = std::fmax(vmax, v);
				vmin = std::fmin(vmin, v);
			}
			vrms = std::sqrt(vrms / BUFFER_SIZE);
			vpp = vmax - vmin;
		}
	};

	Stats statsX, statsY;

	ScopeDisplay() {
		font = APP->window->loadFont(asset::plugin(pluginInstance, FONT));
	}

	void drawWaveform(const DrawArgs &args, float *valuesX, float *valuesY) {
		if (!valuesX)
			return;
		
		nvgSave(args.vg);
		Rect b = Rect(Vec(0, 15), box.size.minus(Vec(0, 15*2)));
		nvgScissor(args.vg, b.pos.x, b.pos.y, b.size.x, b.size.y);
		nvgBeginPath(args.vg);
		// Draw maximum display left to right
		for (int i = 0; i < BUFFER_SIZE; i++) {
			float x, y;
			if (valuesY) {
				x = valuesX[i] / 2.0f + 0.5f;
				y = valuesY[i] / 2.0f + 0.5f;
			}
			else {
				x = (float)i / (BUFFER_SIZE - 1);
				y = valuesX[i] / 2.0f + 0.5f;
			}
			Vec p;
			p.x = b.pos.x + b.size.x * x;
			p.y = b.pos.y + b.size.y * (1.0f - y);
			if (i == 0)
				nvgMoveTo(args.vg, p.x, p.y);
			else
				nvgLineTo(args.vg, p.x, p.y);
		}
		nvgLineCap(args.vg, NVG_ROUND);
		nvgMiterLimit(args.vg, 2.0f);
		nvgStrokeWidth(args.vg, 1.5f);
		nvgGlobalCompositeOperation(args.vg, NVG_LIGHTER);
		nvgStroke(args.vg);
		nvgResetScissor(args.vg);
		nvgRestore(args.vg);
	}

	void drawTrig(const DrawArgs &args, float value) {
		Rect b = Rect(Vec(0, 15), box.size.minus(Vec(0, 15*2)));
		nvgScissor(args.vg, b.pos.x, b.pos.y, b.size.x, b.size.y);

		value = value / 2.0f + 0.5f;
		Vec p = Vec(box.size.x, b.pos.y + b.size.y * (1.0f - value));

		// Draw line
		nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x10));
		{
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, p.x - 13, p.y);
			nvgLineTo(args.vg, 0, p.y);
			nvgClosePath(args.vg);
		}
		nvgStroke(args.vg);

		// Draw indicator
		nvgFillColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x60));
		{
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, p.x - 2, p.y - 4);
			nvgLineTo(args.vg, p.x - 9, p.y - 4);
			nvgLineTo(args.vg, p.x - 13, p.y);
			nvgLineTo(args.vg, p.x - 9, p.y + 4);
			nvgLineTo(args.vg, p.x - 2, p.y + 4);
			nvgClosePath(args.vg);
		}
		nvgFill(args.vg);

		nvgFontSize(args.vg, 9);
		nvgFontFaceId(args.vg, font->handle);
		nvgFillColor(args.vg, nvgRGBA(0x1e, 0x28, 0x2b, 0xff));
		nvgText(args.vg, p.x - 8, p.y + 3, "T", NULL);
		nvgResetScissor(args.vg);
	}

	void drawStats(const DrawArgs &args, Vec pos, const char *title, Stats *stats, NVGcolor color) {
		nvgFontSize(args.vg, FONT_SIZE);
		nvgFontFaceId(args.vg, font->handle);
		nvgTextLetterSpacing(args.vg, LETTER_SPACING);

		//color.a = 0x90;
		nvgFillColor(args.vg, color);
		nvgText(args.vg, pos.x + 6, pos.y + 11, title, NULL);

		nvgFillColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x80));
		char text[128];
		snprintf(text, sizeof(text), "pp % 06.2f  max % 06.2f  min % 06.2f", stats->vpp, stats->vmax, stats->vmin);
		nvgText(args.vg, pos.x + 22, pos.y + 11, text, NULL);
	}

	void draw(const DrawArgs &args) override {
		if (!module)
			return;

		assert(module->xPort);
		assert(module->yPort);

		float gainX = std::pow(2.f, std::round(module->params[Scope::X_SCALE_PARAM].getValue() / 10.f));
		float gainY = std::pow(2.f, std::round(module->params[Scope::Y_SCALE_PARAM].getValue() / 10.f));
		float offsetX = module->params[Scope::X_POS_PARAM].getValue();
		float offsetY = module->params[Scope::Y_POS_PARAM].getValue();

		float valuesX[BUFFER_SIZE];
		float valuesY[BUFFER_SIZE];
		for (int i = 0; i < BUFFER_SIZE; i++) {
			int j = i;
			// Lock display to buffer if buffer update deltaTime <= 2^-11
			if (module->lissajous)
				j = (i + module->bufferIndex) % BUFFER_SIZE;
			valuesX[i] = (module->bufferX[j] + offsetX) * gainX / 10.0f;
			valuesY[i] = (module->bufferY[j] + offsetY) * gainY / 10.0f;
		}

		// Compute colors
		NVGcolor xColor, yColor;
		xColor = getPortColor(module->xPort);
		yColor = getPortColor(module->yPort);
		if (memcmp(&xColor, &yColor, sizeof(xColor)) == 0 ) {
			// if the colors are the same pick novel colors
			NVGcolor tmp = xColor;
			tmp.r = xColor.g;
			tmp.g = xColor.b;
			tmp.b = xColor.r;
			xColor = tmp;
			tmp.r = yColor.b;
			tmp.g = yColor.r;
			tmp.b = yColor.g;
			yColor = tmp;
		}

		// Draw waveforms
		if (module->lissajous) {
			// X x Y
			if (module->inputs[Scope::X_INPUT].isConnected() || module->inputs[Scope::Y_INPUT].isConnected()) {
				nvgStrokeColor(args.vg, getPortColor(nullptr));
				drawWaveform(args, valuesX, valuesY);
			}
		}
		else {
			// Y
			if (module->inputs[Scope::Y_INPUT].isConnected()) {
				nvgStrokeColor(args.vg, yColor);
				drawWaveform(args, valuesY, NULL);
			}

			// X
			if (module->inputs[Scope::X_INPUT].isConnected()) {
				nvgStrokeColor(args.vg, xColor);
				drawWaveform(args, valuesX, NULL);
			}

			float valueTrig = (module->params[Scope::TRIG_PARAM].getValue() + offsetX) * gainX / 10.0f;
			drawTrig(args, valueTrig);
		}

		// Calculate and draw stats
		if (++frame >= 4) {
			frame = 0;
			statsX.calculate(module->bufferX);
			statsY.calculate(module->bufferY);
		}

		NVGcolor statsColor = nvgRGBA(0xff, 0xff, 0xff, 0x40);
		if (!module->inputs[Scope::X_INPUT].isConnected()) {
			xColor = statsColor;
		}
		if (!module->inputs[Scope::Y_INPUT].isConnected()) {
			yColor = statsColor;
		}
		drawStats(args, Vec(0, 0), "X", &statsX, xColor);
		drawStats(args, Vec(0, box.size.y - 15), "Y", &statsY, yColor);
	}

	NVGcolor getPortColor(PortWidget* port) {
		CableWidget *cable = APP->scene->rack->/*cableContainer->*/getTopCable(port);
		NVGcolor color = cable ? cable->color : nvgRGBA(0x9f, 0xe4, 0x36, 0xc0);
		//color.a = 0xc0;
		return color;
	}

};


struct ScopeWidget : ModuleWidget {
	ScopeWidget(Scope *module){

		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/colorScope.svg")));

		addChild(createWidget<ScrewSilver>(Vec(15, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 30, 0)));
		addChild(createWidget<ScrewSilver>(Vec(15, 365)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 30, 365)));
		if (module != NULL) {
			ScopeDisplay *display = new ScopeDisplay();
			display->module = module;
			display->box.pos = Vec(0, 44);
			display->box.size = Vec(box.size.x, 140);
			addChild(display);
		}

		addParam(createParam<RoundBlackSnapKnob>(Vec(15, 209), module, Scope::X_SCALE_PARAM));
		addParam(createParam<RoundBlackKnob>(Vec(15, 263), module, Scope::X_POS_PARAM));
		addParam(createParam<RoundBlackSnapKnob>(Vec(61, 209), module, Scope::Y_SCALE_PARAM));
		addParam(createParam<RoundBlackKnob>(Vec(61, 263), module, Scope::Y_POS_PARAM));
		addParam(createParam<RoundBlackKnob>(Vec(107, 209), module, Scope::TIME_PARAM));
		addParam(createParam<CKD6>(Vec(106, 262), module, Scope::LISSAJOUS_PARAM));
		addParam(createParam<RoundBlackKnob>(Vec(153, 209), module, Scope::TRIG_PARAM));
		addParam(createParam<CKD6>(Vec(152, 262), module, Scope::EXTERNAL_PARAM));

		module->xPort = dynamic_cast<PortWidget*>(createInput<PJ301MPort>(Vec(17, 319), module, Scope::X_INPUT));
		addInput(module->xPort);
		module->yPort = dynamic_cast<PortWidget*>(createInput<PJ301MPort>(Vec(63, 319), module, Scope::Y_INPUT));
		addInput(module->yPort);

		addInput(createInput<PJ301MPort>(Vec(154, 319), module, Scope::TRIG_INPUT));

		addChild(createLight<SmallLight<GreenLight>>(Vec(104, 251), module, Scope::PLOT_LIGHT));
		addChild(createLight<SmallLight<GreenLight>>(Vec(104, 296), module, Scope::LISSAJOUS_LIGHT));
		addChild(createLight<SmallLight<GreenLight>>(Vec(150, 251), module, Scope::INTERNAL_LIGHT));
		addChild(createLight<SmallLight<GreenLight>>(Vec(150, 296), module, Scope::EXTERNAL_LIGHT));
	}
};

//#ifdef PQ_VERSION
//Model *modelScope = createModel<Scope, ScopeWidget>("Scope");
//#else
Model *modelScope = createModel<Scope, ScopeWidget>("GenericScope");
//#endif
