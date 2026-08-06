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

// ── grid ────────────────────────────────────────────────────────────────────
// One grid, one unit: 1HP, in BOTH axes. Controls sit on intersections, so a
// layout is stated in whole cells and scales with the panel instead of being a
// pile of loose millimetre values. A panel is 128.5mm tall = 25.29HP, so there
// are 25 usable rows and the remainder is left as a margin at the FOOT of the
// panel rather than distributed.
static const float HP = 5.08f;
static inline float hp(float n) { return n * HP; }
static const int GRID_ROWS = 25;

// Jacks are 8.03mm and trimpots 6.05mm, so neighbours need 2 cells between them;
// a pot and its own jack sit 2 cells apart on the same row and are joined by a
// hairline, which is what says they belong together.
static const float PAIR_SPAN = 2.f;      // cells from a pot to its jack

// ── type ────────────────────────────────────────────────────────────────────
// One label size across the plugin. Figtree has a far larger x-height than the
// mono face the panels used before, so it reads bigger at the same nominal size
// — hence 4.4mm rather than the old 6mm. NOTE is for the small annotations that
// mark what a pot position means, and is the only other size in use.
static const float TYPE_LABEL = 3.3f;   // mm
static const float TYPE_NOTE  = 2.5f;   // mm — pot-position notes, ranges
static const float TYPE_TITLE = 5.6f;   // mm — the module name

// ── screen type ─────────────────────────────────────────────────────────────
// One size for on-screen text, for the same reason there is one size for panel
// labels. Note and Beat draw every string at 9 units on a 174-unit display,
// which is exactly SCREEN below in mm — stating it here means a new display
// cannot drift away from them by accident, which is precisely what happened
// with Key and Loom (both invented sizes proportional to their own height, so
// each module's text came out a different size).
// SCREEN_SMALL is the screen's counterpart to TYPE_NOTE: dense rows only, where
// the full size genuinely will not fit.
// Neither carries letter spacing — Note and Beat set none, and tracking is what
// made the same nominal size read as a different face.
static const float TYPE_SCREEN       = 2.38f;  // mm
static const float TYPE_SCREEN_SMALL = 1.90f;  // mm

static inline void screenFont(NVGcontext* vg, const std::shared_ptr<Font>& f,
                              float mm = TYPE_SCREEN) {
	nvgFontFaceId(vg, f->handle);
	nvgFontSize(vg, mm2px(mm));
	nvgTextLetterSpacing(vg, 0.f);
}

// ── spacing ─────────────────────────────────────────────────────────────────
// Labels sit ABOVE their control; a jack's label clears the jack by a little
// more because the jack's own bezel reads as part of the shape.
static const float LABEL_GAP_KNOB = 5.6f;    // mm from control centre to label baseline
static const float LABEL_GAP_JACK = 5.4f;
static const float LABEL_GAP_TRIM = 4.4f;
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
	struct Link { Vec a, b; bool onPlate; };
	std::vector<Item> items;
	std::vector<Link> links;
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

	// A pot and the jack that modulates it, tied together. The label goes on the
	// pot; the jack does not need one, because the line already says what it is.
	void pair(float x, float y, const std::string& t, bool onPlate = false) {
		add(x, y - LABEL_GAP_TRIM, t, onPlate ? ON_PLATE : LABEL);
		links.push_back({Vec(x, y), Vec(x + hp(PAIR_SPAN), y), onPlate});
	}
	// The same pairing turned through 90°: the jack sits BELOW its pot rather
	// than beside it, for panels that run their controls down a column. The
	// caller gives both centres, because the vertical gap is the row pitch and
	// that is a layout decision rather than a constant.
	void pairDown(float x, float y, float yJack, const std::string& t,
	              bool onPlate = false) {
		add(x, y - LABEL_GAP_TRIM, t, onPlate ? ON_PLATE : LABEL);
		links.push_back({Vec(x, y), Vec(x, yJack), onPlate});
	}
	void link(float x1, float y1, float x2, float y2, bool onPlate = false) {
		links.push_back({Vec(x1, y1), Vec(x2, y2), onPlate});
	}
	void title(float x, float y, const std::string& t) {
		add(x, y, t, TITLE, NVG_ALIGN_LEFT);
	}

	void draw(const DrawArgs& args) override {
		if (!font || font->handle < 0) font = panelFont();
		if (!bold || bold->handle < 0) bold = panelFontBold();
		if (!font || font->handle < 0) return;
		NVGcontext* vg = args.vg;
		for (const Link& l : links) {          // drawn first, so a component covers its ends
			nvgBeginPath(vg);
			nvgMoveTo(vg, mm2px(l.a.x), mm2px(l.a.y));
			nvgLineTo(vg, mm2px(l.b.x), mm2px(l.b.y));
			nvgStrokeColor(vg, l.onPlate ? nvgRGB(0x4A, 0x4A, 0x52) : nvgRGB(0x9A, 0x9A, 0xA6));
			nvgStrokeWidth(vg, mm2px(0.35f));
			nvgStroke(vg);
		}
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
