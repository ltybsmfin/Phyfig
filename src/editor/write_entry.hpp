#pragma once
// Write-style entry animation for text/formula objects.
//
// The document stores an optional `write` object beside `properties`:
//
//   "write": {"start":0.4,"duration":2.0,"stagger":0.35,"phase":0.65,
//             "outline":true,"outlineWidth":1.5,"fill":true,"easing":"smooth"}
//
// `phase` is the outline's share of each unit window and must stay below 1: the
// fill always needs time left, otherwise the object would look finished the
// moment its window starts.
//
// Everything below is pure CPU code without ImGui, Vulkan or window state, so
// the geometry and the timing can be unit-tested directly.
//
// Two halves:
//   * `buildLayout()` turns coverage masks (glyph bitmaps, formula masks) into
//     cached geometry: closed sub-pixel outlines and fill spans per connected
//     region, plus a deterministic unit order.
//   * `evaluate()` is a pure function of (spec, document time). A frame never
//     depends on earlier frames, so playback, arbitrary seeks (including
//     backwards) and the offscreen export all agree.
//
// The outline is extracted from the rendered coverage mask, not from the real
// vector paths of the font. For CJK that means a glyph is broken into the
// connected ink regions found in the bitmap and drawn in top-to-bottom,
// left-to-right order: this is *not* the real stroke order of the character.
#include "document.hpp"

#include <cstddef>
#include <vector>

namespace physics_editor::write_entry {

// Raster size the text outlines are traced at. Fixed on purpose: the geometry
// cache then only depends on the text, never on the view zoom or the drawn pixel
// size, and Chinese glyphs get a smooth silhouette instead of atlas steps.
constexpr float kDefaultRasterPx = 64.f;

struct Point { float x = 0.f, y = 0.f; };
// One closed outline in mask pixel space (x right, y down). `points` never
// repeats the first point at the end. Length and signed area are precomputed.
struct Loop { std::vector<Point> points; float length = 0.f, area = 0.f; };
struct Span { float x0 = 0.f, x1 = 0.f; };
// One connected ink region. `rows[y - minY]` holds the texel spans this region
// draws: every texel above zero coverage that belongs to it — antialiased fringe
// included — and never a texel of another region, so a fill can sample the
// original mask alpha exactly once and holes need no even-odd work. The spans also
// carry one ring of zero coverage texels around the ink, cut off at the mask
// border: those texels draw nothing by themselves (the fill multiplies the sampled
// alpha), but they are what the bilinear filter reads for the soft edge just
// outside the outermost ink, so the fill shows the same edge the finished draw
// does. `minX..maxY` bound the drawn spans and `inkMinX..inkMaxY` bound the
// coverage they were built from; the drawing order uses the ink bounds, so
// widening the draw range can never reshuffle the animation.
struct Component {
    float minX = 0.f, minY = 0.f, maxX = 0.f, maxY = 0.f;
    float inkMinX = 0.f, inkMinY = 0.f, inkMaxX = 0.f, inkMaxY = 0.f;
    std::vector<Loop> loops;              // outer boundary first, then holes
    std::vector<std::vector<Span>> rows;  // fill spans per pixel row, starting at minY
    float strokeLength = 0.f;
};

// One staggered step: a character for text, one glyph or rule of a formula
// (which may consist of several ink regions: `=`, `i`, `%`). A region whose
// coverage never reaches the ink level still becomes a unit: the fill shows it,
// only its outline phase stays empty.
struct Unit {
    std::vector<Component> components;
    float strokeLength = 0.f;
};
struct Layout { std::vector<Unit> units; float totalLength = 0.f; };

// 8-bit coverage, row major, `width * height` bytes.
struct Mask { const unsigned char* alpha = nullptr; int width = 0, height = 0; };

// One unit of a writing plan: which ink components form one staggered step. The
// components are indices into the mask's connected regions in raster-scan order
// (the same enumeration the tracer uses), so a plan can only ever regroup and
// reorder the regions that are really there.
enum class PlanKind { Glyph, Rule };
struct PlanUnitView { const int* components = nullptr; int count = 0; PlanKind kind = PlanKind::Glyph; };

// Optional structural order for a formula mask, generated from the same PDF the
// mask was rendered from. A fraction is written bar first, a base is followed by
// its scripts and a radical stays next to its radicand — none of which the raster
// reading order can express. The plan is used only when it describes exactly the
// mask at hand (see buildLayout); otherwise the reading order stays in charge.
struct PlanView {
    int width = 0, height = 0;  // mask size the plan was generated for
    int componentCount = 0;     // ink components of that mask
    const PlanUnitView* units = nullptr;
    int unitCount = 0;
};

enum class UnitMode {
    // All regions of one mask form a single unit (one text character).
    SingleUnit,
    // Every connected region becomes its own unit, ordered in reading order
    // (bands top to bottom, then left to right). Used for formulas without a
    // writing plan; with one, the plan's order and grouping are used instead.
    ComponentPerUnit,
};
struct UnitRequest { Mask mask; float x = 0.f, y = 0.f; UnitMode mode = UnitMode::SingleUnit; PlanView plan{}; };

// Geometry is built in mask pixels; `x`/`y` place a mask inside the shared
// unit coordinate space (for text: the pen position of that character).
// Empty requests still produce an (empty) unit so a character without ink —
// a space, or a code point no installed font covers — keeps its place in the
// stagger cadence.
Layout buildLayout(const std::vector<UnitRequest>& requests);

struct UnitPhase { float stroke = 0.f, fill = 0.f; };
struct Progress {
    // 0 at `start`, 1 at `start + duration`. The caller draws the normal glyph
    // for exactly 1 and nothing at all for exactly 0.
    float total = 0.f;
    // Phase switches from the spec, so the renderer never re-reads the JSON.
    bool outline = true, fill = true;
    std::vector<UnitPhase> units;
};

bool hasWrite(const Json& object);
// `write` with every default filled in; throws when the spec is invalid.
Json resolved(const Json& object);
// Pure function of time. `unitCount` must match the cached layout. The scene
// duration only constrains the window at validation time, never at draw time.
Progress evaluate(const Json& object, double time, size_t unitCount);
// Document validation: object type, field set, bounds and the scene duration.
void validate(const Json& object, double sceneDuration);

// Ready-made scene with Chinese text and two formulas, used by the editor's
// example menu and by `scenes/write-entry.json`.
Json demo();
}
