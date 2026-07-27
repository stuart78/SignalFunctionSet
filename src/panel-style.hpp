#pragma once
// =============================================================================
// Panel style — the design system, as code.
//
// Panels used to be drawn in Illustrator, which meant every label had to be
// outlined to paths (Rack ignores <text> in a panel SVG). Chance carries 84 text
// paths, Band 106; changing one word meant going back to Illustrator. So text is
// drawn at runtime instead, in Figtree, from this one place — and the SVG holds
// only geometry.
//
// What is actually VISIBLE on a finished panel is a shorter list than it looks,
// because Rack draws each component on top of its own footprint: anything in the
// SVG the size of a knob or a jack is hidden underneath it. The design is
// therefore the background, the plates, the screens, the labels, and any mark
// that sits OUTSIDE a component's footprint.
// =============================================================================

#include "plugin.hpp"
#include <string>
#include <vector>

namespace sfs {

// ── palette ─────────────────────────────────────────────────────────────────
static const NVGcolor PANEL      = nvgRGB(0xF0, 0xF0, 0xF0);  // the faceplate
static const NVGcolor INK        = nvgRGB(0x23, 0x1F, 0x20);  // label text on the faceplate
static const NVGcolor INK_SOFT   = nvgRGB(0x6A, 0x6A, 0x78);  // sub-labels, pot-position notes
static const NVGcolor PLATE      = nvgRGB(0x1A, 0x1A, 0x1A);  // dark inset grouping a section
static const NVGcolor PLATE_INK  = nvgRGB(0xE8, 0xE8, 0xF0);  // label text on a plate
static const NVGcolor HAIRLINE   = nvgRGB(0xB2, 0xB2, 0xB2);  // connectors, reticules

// display palette — what the screens clear to and draw with
static const NVGcolor SCREEN_BG   = nvgRGB(0x1A, 0x1A, 0x32);
static const NVGcolor SCREEN_BLUE = nvgRGB(0x00, 0x97, 0xDE);
static const NVGcolor SCREEN_DEEP = nvgRGB(0x0D, 0x59, 0x86);
static const NVGcolor SCREEN_LINE = nvgRGB(0x0D, 0x59, 0x88);
static const NVGcolor SCREEN_PURP = nvgRGB(0x35, 0x35, 0x4D);
static const NVGcolor SCREEN_PMID = nvgRGB(0x4A, 0x4A, 0x66);
static const NVGcolor SCREEN_HOT  = nvgRGB(0xEC, 0x65, 0x2E);
static const NVGcolor SCREEN_TEXT = nvgRGB(0xE8, 0xE8, 0xF0);
static const NVGcolor SCREEN_DIM  = nvgRGB(0x8A, 0x8A, 0xA5);

// ── type ────────────────────────────────────────────────────────────────────
// One label size across the plugin. Figtree has a far larger x-height than the
// mono face the panels used before, so it reads bigger at the same nominal size
// — hence 4.4mm rather than the old 6mm. NOTE is for the small annotations that
// mark what a pot position means, and is the only other size in use.
static const float TYPE_LABEL = 4.4f;   // mm
static const float TYPE_NOTE  = 3.2f;   // mm — pot-position notes, ranges
static const float TYPE_TITLE = 7.0f;   // mm — the module name

// ── spacing ─────────────────────────────────────────────────────────────────
// Labels sit ABOVE their control; a jack's label clears the jack by a little
// more because the jack's own bezel reads as part of the shape.
static const float LABEL_GAP_KNOB = 6.6f;    // mm from control centre to label baseline
static const float LABEL_GAP_JACK = 6.4f;
static const float LABEL_GAP_TRIM = 5.2f;
static const float PLATE_PAD      = 2.6f;    // inset padding around a grouped section
static const float PLATE_RADIUS   = 2.0f;

static inline std::shared_ptr<Font> panelFont() {
	return APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Figtree-Regular.ttf"));
}
static inline std::shared_ptr<Font> panelFontBold() {
	return APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Figtree-SemiBold.ttf"));
}

// ── labels ──────────────────────────────────────────────────────────────────
// Positions are in mm and are the CONTROL's centre; the widget applies the gap,
// so moving a control means changing one number in the widget constructor and
// nothing else.
struct PanelLabels : Widget {
	enum Kind { LABEL, NOTE, TITLE, ON_PLATE };
	struct Item { Vec pos; std::string text; Kind kind; int align; };
	std::vector<Item> items;
	std::shared_ptr<Font> font, bold;

	void add(float x, float y, const std::string& t, Kind k = LABEL,
	         int align = NVG_ALIGN_CENTER) {
		items.push_back({Vec(x, y), t, k, align});
	}
	// the common cases, so a caller states the control position and nothing else
	void knob(float x, float y, const std::string& t)  { add(x, y - LABEL_GAP_KNOB, t); }
	void trim(float x, float y, const std::string& t)  { add(x, y - LABEL_GAP_TRIM, t); }
	void jack(float x, float y, const std::string& t)  { add(x, y - LABEL_GAP_JACK, t); }
	void jackOnPlate(float x, float y, const std::string& t) {
		add(x, y - LABEL_GAP_JACK, t, ON_PLATE);
	}
	void note(float x, float y, const std::string& t)  { add(x, y, t, NOTE); }
	void title(float x, float y, const std::string& t) {
		add(x, y, t, TITLE, NVG_ALIGN_LEFT);
	}

	void draw(const DrawArgs& args) override {
		if (!font || font->handle < 0) font = panelFont();
		if (!bold || bold->handle < 0) bold = panelFontBold();
		if (!font || font->handle < 0) return;
		NVGcontext* vg = args.vg;
		for (const Item& it : items) {
			bool isTitle = it.kind == TITLE;
			nvgFontFaceId(vg, isTitle && bold && bold->handle >= 0 ? bold->handle : font->handle);
			nvgFontSize(vg, mm2px(it.kind == NOTE ? TYPE_NOTE
			                    : isTitle         ? TYPE_TITLE : TYPE_LABEL));
			nvgTextLetterSpacing(vg, mm2px(it.kind == NOTE ? 0.04f : 0.10f));
			nvgFillColor(vg, it.kind == ON_PLATE ? PLATE_INK
			               : it.kind == NOTE     ? INK_SOFT : INK);
			nvgTextAlign(vg, it.align | NVG_ALIGN_MIDDLE);
			nvgText(vg, mm2px(it.pos.x), mm2px(it.pos.y), it.text.c_str(), NULL);
		}
		Widget::draw(args);
	}
};

}  // namespace sfs
