// Write entry tests: outline extraction, unit order, timing boundaries, the
// document contract (command path, history, serialization) and the CJK chain
// through the real system font.
//
// Registered as the CTest `editor_write_entry` test by apps/editor/CMakeLists.txt.
// The ImGui sources are compiled into this target because rasterizeText() builds
// a throwaway CPU font atlas; no GPU, window or Vulkan device is touched.
#include "write_entry.hpp"

#include "component.hpp"
#include "font_support.hpp"
#include "formulas/formula_cache.hpp"
#include "formulas/formula_writing_plan.hpp"
#include "formula_plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace physics_editor;
namespace we = physics_editor::write_entry;

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void checkNear(double value, double expected, double tolerance, const char* message) {
    if (!(std::abs(value - expected) <= tolerance)) {
        throw std::runtime_error(std::string(message) + " (got " + std::to_string(value) + ", expected " + std::to_string(expected) + ")");
    }
}
template <class F> void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, message);
}

struct Mask8 {
    int width = 0, height = 0;
    std::vector<unsigned char> alpha;
    Mask8(int w, int h) : width(w), height(h), alpha(static_cast<std::size_t>(w) * h, 0) {}
    void rect(int x0, int y0, int x1, int y1, unsigned char value = 255) {
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) alpha[static_cast<std::size_t>(y) * width + x] = value;
    }
    we::Mask view() const { return {alpha.data(), width, height}; }
};

double contourMinX(const we::Loop& loop) {
    double value = loop.points.front().x;
    for (const auto& point : loop.points) value = std::min(value, static_cast<double>(point.x));
    return value;
}
double contourMaxX(const we::Loop& loop) {
    double value = loop.points.front().x;
    for (const auto& point : loop.points) value = std::max(value, static_cast<double>(point.x));
    return value;
}

// The fill has to draw exactly the texels the bilinear filter of the finished draw
// can reach:
//   * a texel with coverage is drawn exactly once, so no ink vanishes from an
//     animated frame and no texel is blended twice,
//   * a texel without coverage is drawn exactly when it touches a covered texel in
//     its eight neighbourhood — one texel further than the filter taps reach, and
//     never a texel no tap can touch,
//   * no span leaves the mask, because a UV outside the mask reads whatever the
//     atlas stores next to the mask.
// `offsetX/offsetY` undo the placement of the layout under test.
void checkOwnership(const we::Layout& layout, const Mask8& mask, float offsetX, float offsetY, const char* message) {
    std::vector<int> count(mask.alpha.size(), 0);
    for (const auto& unit : layout.units) {
        for (const auto& component : unit.components) {
            for (std::size_t row = 0; row < component.rows.size(); ++row) {
                const int y = static_cast<int>(component.minY - offsetY) + static_cast<int>(row);
                check(y >= 0 && y < mask.height, "A fill row must stay inside the mask");
                for (const auto& span : component.rows[row]) {
                    const int first = static_cast<int>(span.x0 - offsetX), last = static_cast<int>(span.x1 - offsetX);
                    check(first >= 0 && last <= mask.width && first <= last, "A fill span must stay inside its row");
                    for (int x = first; x < last; ++x) count[static_cast<std::size_t>(y) * mask.width + x] += 1;
                }
            }
        }
    }
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * mask.width + x;
            bool touchesInk = mask.alpha[index] > 0;
            for (int dy = -1; dy <= 1 && !touchesInk; ++dy) {
                for (int dx = -1; dx <= 1 && !touchesInk; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= mask.width || ny >= mask.height) continue;
                    touchesInk = mask.alpha[static_cast<std::size_t>(ny) * mask.width + nx] > 0;
                }
            }
            const int wanted = touchesInk ? 1 : 0;
            if (count[index] != wanted) throw std::runtime_error(std::string(message) + " (texel " + std::to_string(index) +
                " at " + std::to_string(x) + "," + std::to_string(y) + " covered " + std::to_string(count[index]) +
                " times, expected " + std::to_string(wanted) + ")");
        }
    }
}

// Bilinear tap of the mask, read the way the finished whole quad draw reads it:
// texel centres are at (i + 0.5). Samples whose four taps would leave the mask are
// skipped, because outside the mask the real draw reads the atlas instead.
bool sampledAlpha(const Mask8& mask, float tx, float ty, float* value) {
    const float x = tx - 0.5f, y = ty - 0.5f;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    if (x0 < 0 || y0 < 0 || x0 + 1 >= mask.width || y0 + 1 >= mask.height) return false;
    const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
    *value = 0.f;
    for (int j = 0; j <= 1; ++j) {
        for (int i = 0; i <= 1; ++i) {
            const float weight = (i != 0 ? fx : 1.f - fx) * (j != 0 ? fy : 1.f - fy);
            *value += weight * static_cast<float>(mask.alpha[static_cast<std::size_t>(y0 + j) * mask.width + x0 + i]);
        }
    }
    return true;
}

// Is the texel a sub-texel position falls into covered by one of the drawn spans?
bool drawnAt(const we::Layout& layout, float tx, float ty, float offsetX, float offsetY) {
    const int x = static_cast<int>(std::floor(tx - offsetX)), y = static_cast<int>(std::floor(ty - offsetY));
    for (const auto& unit : layout.units) {
        for (const auto& component : unit.components) {
            const int row = y - static_cast<int>(component.minY - offsetY);
            if (row < 0 || row >= static_cast<int>(component.rows.size())) continue;
            for (const auto& span : component.rows[static_cast<std::size_t>(row)]) {
                if (static_cast<float>(x) >= span.x0 - offsetX && static_cast<float>(x) < span.x1 - offsetX) return true;
            }
        }
    }
    return false;
}

void testSubPixelOutline() {
    Mask8 mask(24, 24);
    mask.rect(5, 5, 14, 14);
    const auto layout = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    check(layout.units.size() == 1, "One mask must stay one unit");
    const auto& unit = layout.units[0];
    check(unit.components.size() == 1, "A single square must be one component");
    check(unit.components[0].loops.size() == 1, "A square has exactly one outline");
    const auto& loop = unit.components[0].loops[0];
    // The coverage boundary sits on the texel boundary of the drawn bitmap, so a
    // solid block of pixels 5..14 is outlined from 5.0 to 15.0. Corners are
    // chamfered by half a pixel, which is why the perimeter is 4 * (9 + sqrt(2)/2).
    checkNear(contourMinX(loop), 5.0, 0.05, "Outline must follow the texel boundary");
    checkNear(contourMaxX(loop), 15.0, 0.05, "Outline must follow the texel boundary");
    checkNear(loop.length, 4.0 * (9.0 + std::sqrt(2.0) / 2.0), 0.3, "Perimeter must follow the interpolated outline");
    checkNear(unit.strokeLength, loop.length, 1e-3, "Unit stroke length must include every contour");
    // Fill spans come from the label map: pixel columns 5..14 of rows 5..14, plus
    // the one texel filter ring the spans need around that ink.
    const auto& component = unit.components[0];
    check(component.rows.size() == 12, "Fill spans must cover the ink rows and the filter ring around them");
    check(component.rows.front().size() == 1, "A solid row has one span");
    checkNear(component.rows.front().front().x0, 4.0, 1e-4, "The ring row starts one texel before the ink");
    checkNear(component.rows.front().front().x1, 16.0, 1e-4, "The ring row ends one texel after the ink");
    checkNear(component.rows[1].front().x0, 4.0, 1e-4, "An ink row carries the ring on both sides too");
    checkNear(component.rows[1].front().x1, 16.0, 1e-4, "An ink row carries the ring on both sides too");
    checkNear(component.minX, 4.0, 1e-4, "The drawn bounds must include the filter ring");
    checkNear(component.maxY, 15.0, 1e-4, "The drawn bounds must include the filter ring");
    checkNear(component.inkMinX, 5.0, 1e-4, "The ink bounds must match the ink");
    checkNear(component.inkMaxY, 14.0, 1e-4, "The ink bounds must match the ink");
    checkOwnership(layout, mask, 0.f, 0.f, "A block must draw its ink and the filter ring around it");

    // A soft edge must interpolate at the coverage level instead of snapping to a
    // pixel boundary: alpha 100 (background) next to alpha 200 (ink) crosses 128
    // at t = 0.28 through the pair, i.e. at texel coordinate 4.78.
    Mask8 soft(24, 24);
    for (int y = 5; y <= 14; ++y) {
        soft.alpha[static_cast<std::size_t>(y) * soft.width + 5] = 200;
        for (int x = 6; x <= 14; ++x) soft.alpha[static_cast<std::size_t>(y) * soft.width + x] = 255;
    }
    for (int y = 5; y <= 14; ++y) soft.alpha[static_cast<std::size_t>(y) * soft.width + 4] = 100;
    const auto softLayout = we::buildLayout({{soft.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    checkNear(contourMinX(softLayout.units[0].components[0].loops[0]), 4.78, 0.03,
        "A soft coverage edge must interpolate sub-pixel");
}

void testMaskOffset() {
    // A non-zero placement must shift the traced contours together with the
    // bounds and the spans. An unshifted loop drew every stroke at the mask
    // origin while the fill advanced per character.
    Mask8 mask(16, 12);
    mask.rect(2, 3, 9, 9);
    const float offsetX = 37.f, offsetY = 11.f;
    const auto layout = we::buildLayout({{mask.view(), offsetX, offsetY, we::UnitMode::SingleUnit}});
    const auto& component = layout.units[0].components[0];
    checkNear(component.inkMinX, offsetX + 2.f, 1e-4, "Ink bounds must carry the placement offset");
    checkNear(component.inkMinY, offsetY + 3.f, 1e-4, "Ink bounds must carry the placement offset");
    checkNear(component.minX, offsetX + 1.f, 1e-4, "Drawn bounds must carry the placement offset and the ring");
    checkNear(component.minY, offsetY + 2.f, 1e-4, "Drawn bounds must carry the placement offset and the ring");
    check(!component.loops.empty(), "The placed mask must keep its outline");
    for (const auto& loop : component.loops) {
        check(contourMinX(loop) >= offsetX - 1.f, "An outline must not stay at the mask origin (x)");
        for (const auto& point : loop.points) {
            check(point.x >= component.minX - 1.5f && point.x <= component.maxX + 1.5f,
                "Every outline vertex must be placed with its component (x)");
            check(point.y >= component.minY - 1.5f && point.y <= component.maxY + 1.5f,
                "Every outline vertex must be placed with its component (y)");
        }
    }
    // Spans and contours must describe the same placement.
    checkNear(component.rows[1].front().x0, offsetX + 1.f, 1e-4, "Spans must carry the placement offset");
    checkOwnership(layout, mask, offsetX, offsetY, "A placed mask must still own every texel exactly once");
}

void testFringeOwnership() {
    // A soft edge belongs to the region next to it: the fill can then sample the
    // original coverage instead of a thresholded copy, and the fringe is never
    // claimed by two regions.
    Mask8 soft(24, 24);
    for (int y = 5; y <= 14; ++y) {
        soft.alpha[static_cast<std::size_t>(y) * soft.width + 4] = 100;   // fringe
        soft.alpha[static_cast<std::size_t>(y) * soft.width + 5] = 200;   // ink
        for (int x = 6; x <= 14; ++x) soft.alpha[static_cast<std::size_t>(y) * soft.width + x] = 255;
    }
    const auto layout = we::buildLayout({{soft.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    const auto& component = layout.units[0].components[0];
    checkNear(component.inkMinX, 4.0, 1e-4, "The antialiased fringe must belong to the region");
    checkNear(component.minX, 3.0, 1e-4, "The drawn spans also carry the filter ring outside the fringe");
    checkNear(component.rows[1].front().x0, 3.0, 1e-4, "The fill span starts at the filter ring before the fringe");
    checkNear(component.rows[1].front().x1, 16.0, 1e-4, "The fill span ends at the filter ring after the ink");
    checkOwnership(layout, soft, 0.f, 0.f, "A soft edge must be owned exactly once");

    // Two regions separated by a one texel fringe column that touches both: the
    // fringe texel is claimed by exactly one of them, so the fill never blends it
    // twice and neither region swallows the other's ink.
    Mask8 pair(24, 10);
    pair.rect(2, 2, 8, 7);
    pair.rect(10, 2, 16, 7);
    for (int y = 2; y <= 7; ++y) pair.alpha[static_cast<std::size_t>(y) * pair.width + 9] = 90;
    const auto paired = we::buildLayout({{pair.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    check(paired.units[0].components.size() == 2, "A fringe column must not merge two regions");
    checkOwnership(paired, pair, 0.f, 0.f, "A shared fringe column must be owned exactly once");
}

void testDisjointContoursAreNotBridged() {
    Mask8 mask(30, 12);
    mask.rect(2, 2, 9, 9);    // left blob
    mask.rect(20, 2, 27, 9);  // right blob, separated by ten empty columns
    const auto layout = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    const auto& unit = layout.units[0];
    check(unit.components.size() == 2, "Two disjoint ink regions must stay two components");
    check(unit.components[0].minX < unit.components[1].minX, "Components are ordered left to right");
    for (const auto& component : unit.components) {
        for (const auto& loop : component.loops) {
            const bool left = contourMaxX(loop) <= 10.0;
            const bool right = contourMinX(loop) >= 19.0;
            check(left || right, "A contour must never bridge the gap between two ink regions");
        }
    }
    // Two separate 8x8 ink blocks: 2 * 4 * (7 + sqrt(2)/2).
    checkNear(unit.strokeLength, 2.0 * 4.0 * (7.0 + std::sqrt(2.0) / 2.0), 0.3, "Disjoint outlines keep their own perimeter");
}

void testDiagonalPixelsStaySeparate() {
    Mask8 mask(12, 12);
    mask.rect(5, 5, 5, 5);
    mask.rect(6, 6, 6, 6);  // touches only at a corner
    const auto layout = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    check(layout.units[0].components.size() == 2, "Diagonally touching ink is not one connected region");
}

void testHolesAreExcludedFromTheFill() {
    Mask8 mask(24, 24);
    for (int y = 0; y < 24; ++y) for (int x = 0; x < 24; ++x) {
        const double dx = x - 11.5, dy = y - 11.5;
        const double radius = std::sqrt(dx * dx + dy * dy);
        if (radius >= 4.0 && radius <= 9.0) mask.alpha[static_cast<std::size_t>(y) * 24 + x] = 255;
    }
    const auto layout = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    const auto& component = layout.units[0].components[0];
    check(component.loops.size() == 2, "A ring has an outer outline and a hole outline");
    check(std::abs(component.loops[0].area) > std::abs(component.loops[1].area), "The outer outline comes first");
    const std::size_t middle = static_cast<std::size_t>(11.5 - component.minY);
    check(middle < component.rows.size(), "The ring must have a middle row");
    check(component.rows[middle].size() == 2, "A row through the hole has two spans");
    // The filter ring reaches exactly one texel into the hole — that texel is where
    // the taps of the soft inner edge live — and the interior behind it stays
    // empty, so the fill leaves the hole alone.
    checkNear(component.rows[middle][0].x1, 9.0, 1e-4, "The ring reaches exactly one texel into the hole");
    checkNear(component.rows[middle][1].x0, 15.0, 1e-4, "The ring reaches exactly one texel into the hole");
    check(component.rows[middle][0].x1 < component.rows[middle][1].x0, "The hole must not be filled");
    checkOwnership(layout, mask, 0.f, 0.f, "A hole must own no texel while the ring owns every ink texel once");
}

void testFillCoversTheFilterSupport() {
    // The fill samples the original mask, so a screen pixel up to half a texel
    // outside the outermost ink still receives part of it. Every texel whose eight
    // neighbourhood touches ink therefore has to be drawn — one ring is exactly the
    // filter support — and that is what makes the last animated frame agree with
    // the finished whole quad draw that follows it.
    Mask8 mask(20, 20);
    mask.rect(6, 7, 9, 11);  // a 4 x 5 block
    const auto layout = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    const auto& component = layout.units[0].components[0];
    check(component.rows.size() == 7, "The drawn rows are the ink rows plus one texel on both sides");
    checkNear(component.rows.front().front().x0, 5.0, 1e-4, "The ring row starts one texel left of the ink");
    checkNear(component.rows.front().front().x1, 11.0, 1e-4, "The ring row ends one texel right of the ink");
    checkOwnership(layout, mask, 0.f, 0.f, "The filter ring must be drawn for every ink texel");

    // The same requirement stated as the filter itself: wherever a bilinear tap of
    // the mask reaches ink, the fill must have drawn the texel that tap falls into.
    // One ring is enough and nothing further is needed.
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            for (int oy = -2; oy <= 2; ++oy) {
                for (int ox = -2; ox <= 2; ++ox) {
                    const float tx = static_cast<float>(x) + 0.25f * static_cast<float>(ox);
                    const float ty = static_cast<float>(y) + 0.25f * static_cast<float>(oy);
                    float alpha = 0.f;
                    if (!sampledAlpha(mask, tx, ty, &alpha) || alpha <= 0.f) continue;
                    check(drawnAt(layout, tx, ty, 0.f, 0.f),
                        "Every position the bilinear filter can see ink at must be covered by a drawn texel");
                }
            }
        }
    }
}

void testFaintInkIsNeverDropped() {
    // A group of texels whose coverage never reaches the ink level has no label, so
    // it is not a shape the outline can trace. It still carries ink and the finished
    // draw shows it, so the fill must not drop it: it becomes a component of its own.
    Mask8 mask(40, 16);
    mask.rect(8, 4, 13, 11);                                              // the labelled region
    for (int y = 4; y <= 11; ++y) for (int x = 4; x <= 7; ++x) mask.alpha[static_cast<std::size_t>(y) * mask.width + x] = 70;
    for (int y = 5; y <= 7; ++y) for (int x = 30; x <= 32; ++x) mask.alpha[static_cast<std::size_t>(y) * mask.width + x] = 55;
    const auto layout = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    check(layout.units[0].components.size() == 2, "Faint ink stays one component: attached fringe joins, isolated ink stands alone");
    // Four faint texels on the left of the region run against the scan order, so the
    // two rounds only reach two of them: the worklist sweep has to attribute the
    // rest, otherwise the outermost coverage would vanish from every animated frame.
    const auto& attached = layout.units[0].components[0];
    checkNear(attached.inkMinX, 4.0, 1e-4, "A wide faint fringe is attributed to the region beside it");
    checkNear(attached.inkMaxX, 13.0, 1e-4, "The labelled region keeps its ink bounds");
    const auto& isolated = layout.units[0].components[1];
    checkNear(isolated.inkMinX, 30.0, 1e-4, "An isolated faint region keeps its own coverage");
    checkNear(isolated.inkMaxX, 32.0, 1e-4, "An isolated faint region keeps its own coverage");
    check(isolated.loops.empty(), "Coverage below the ink level has no traced outline");
    check(isolated.strokeLength == 0.f, "A region without an outline has no stroke to draw");
    checkOwnership(layout, mask, 0.f, 0.f, "Faint ink must be drawn exactly like strong ink");
}

void testBorderAndHoleBoundaries() {
    // A region that touches the mask border grows no ring there: a span beyond the
    // mask would sample a UV outside it, which is whatever the atlas stores next to
    // this mask. Every span has to stay inside.
    Mask8 full(16, 12);
    full.rect(0, 0, 15, 11);
    const auto bordered = we::buildLayout({{full.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    const auto& block = bordered.units[0].components[0];
    check(block.rows.size() == 12, "A region on the border grows no ring outside the mask");
    checkNear(block.minX, 0.0, 1e-4, "The drawn bounds stop at the mask border");
    checkNear(block.maxX, 15.0, 1e-4, "The drawn bounds stop at the mask border");
    checkNear(block.minY, 0.0, 1e-4, "The drawn bounds stop at the mask border");
    checkNear(block.maxY, 11.0, 1e-4, "The drawn bounds stop at the mask border");
    checkOwnership(bordered, full, 0.f, 0.f, "A region on the border must keep every span inside the mask");

    // A hole is a texel that carries no region id, so nothing has to be subtracted
    // at draw time. It also has to survive the filter ring around it.
    Mask8 holed(20, 20);
    holed.rect(2, 2, 17, 17);
    holed.rect(8, 8, 11, 11, 0);
    const auto layout = we::buildLayout({{holed.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    const auto& frame = layout.units[0].components[0];
    const std::size_t row = static_cast<std::size_t>(9 - static_cast<int>(frame.minY));
    check(row < frame.rows.size(), "The frame must have a row through the hole");
    check(frame.rows[row].size() == 2, "A row through the hole has a span on either side");
    checkNear(frame.rows[row][0].x1, 9.0, 1e-4, "The ring reaches exactly one texel into the hole");
    checkNear(frame.rows[row][1].x0, 11.0, 1e-4, "The ring reaches exactly one texel into the hole");
    checkOwnership(layout, holed, 0.f, 0.f, "A hole keeps no region while the frame owns every texel once");
}

void testUnitOrder() {
    // One unit: ink regions are drawn top to bottom, then left to right.
    Mask8 mask(30, 30);
    mask.rect(14, 2, 20, 8);   // top right
    mask.rect(2, 2, 8, 8);     // top left
    mask.rect(2, 16, 8, 22);   // bottom left
    const auto layout = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::SingleUnit}});
    check(layout.units.size() == 1, "SingleUnit keeps one unit");
    const auto& components = layout.units[0].components;
    check(components.size() == 3, "Three ink regions expected");
    check(components[0].minX < 10.f && components[0].minY < 10.f, "Top left region comes first");
    check(components[1].minX > 10.f && components[1].minY < 10.f, "Top right region comes second");
    check(components[2].minY > 10.f, "Bottom region comes last");

    // Formula order: bands top to bottom (numerator, bar, denominator), left to
    // right inside a band — a stable, explainable order without parsing LaTeX.
    Mask8 fraction(24, 24);
    fraction.rect(6, 2, 14, 6);    // numerator
    fraction.rect(2, 10, 21, 11);  // fraction bar
    fraction.rect(6, 16, 14, 20);  // denominator
    const auto units = we::buildLayout({{fraction.view(), 0.f, 0.f, we::UnitMode::ComponentPerUnit}});
    check(units.units.size() == 3, "One unit per connected region for formulas");
    check(units.units[0].components[0].minY < 8.f, "Numerator first");
    check(units.units[1].components[0].minY > 8.f && units.units[1].components[0].minY < 14.f, "Bar second");
    check(units.units[2].components[0].minY > 14.f, "Denominator last");

    // An empty mask still keeps its unit so the stagger cadence stays stable.
    Mask8 empty(8, 8);
    const auto blank = we::buildLayout({{empty.view(), 0.f, 0.f, we::UnitMode::ComponentPerUnit}});
    check(blank.units.size() == 1, "An empty formula mask keeps one unit");
}

// How many ink texels a layout really draws: every span texel whose coverage
// reaches the ink level. The plan only regroups regions, so the number has to
// stay exactly the mask's ink texel count.
std::size_t drawnInkTexels(const we::Layout& layout, const Mask8& mask) {
    std::size_t total = 0;
    for (const auto& unit : layout.units) {
        for (const auto& component : unit.components) {
            for (std::size_t row = 0; row < component.rows.size(); ++row) {
                const int y = static_cast<int>(component.minY) + static_cast<int>(row);
                if (y < 0 || y >= mask.height) throw std::runtime_error("A fill row must stay inside the mask");
                for (const auto& span : component.rows[row]) {
                    for (int x = static_cast<int>(span.x0); x < static_cast<int>(span.x1); ++x) {
                        if (mask.alpha[static_cast<std::size_t>(y) * mask.width + x] >= 128) ++total;
                    }
                }
            }
        }
    }
    return total;
}

std::size_t maskInkTexels(const Mask8& mask) {
    std::size_t total = 0;
    for (unsigned char value : mask.alpha) if (value >= 128) ++total;
    return total;
}

const formula_cache::Entry& builtin(const char* name) {
    for (const auto& entry : formula_cache::entries) if (std::strcmp(name, entry.name) == 0) return entry;
    throw std::runtime_error(std::string("Unknown built-in formula: ") + name);
}

Mask8 decodeBuiltin(const formula_cache::Entry& entry) {
    Mask8 mask(entry.width, entry.height);
    std::size_t position = 0;
    for (std::size_t index = 0; index < entry.bytes; index += 2) {
        for (int repeat = 0; repeat < entry.rle[index]; ++repeat) {
            check(position < mask.alpha.size(), "Built-in mask overflow");
            mask.alpha[position++] = entry.rle[index + 1];
        }
    }
    check(position == mask.alpha.size(), "Incomplete built-in mask");
    return mask;
}

const formula_writing_plan::Entry& builtinPlan(const char* name) {
    for (const auto& entry : formula_writing_plan::entries) if (std::strcmp(name, entry.name) == 0) return entry;
    throw std::runtime_error(std::string("Unknown built-in writing plan: ") + name);
}

we::PlanView planView(const formula_writing_plan::Entry& entry, std::vector<we::PlanUnitView>& storage) {
    storage.clear();
    for (int index = 0; index < entry.unitCount; ++index) {
        storage.push_back({entry.units[index].components, entry.units[index].count,
            entry.units[index].kind == 1 ? we::PlanKind::Rule : we::PlanKind::Glyph});
    }
    return {entry.width, entry.height, entry.componentCount, storage.data(), entry.unitCount};
}

// The writing plan replaces the raster reading order: the bar of a fraction is
// drawn first, then its numerator and denominator, and one plan unit may own
// several ink regions (the two bars of `=`).
void testWritingPlanOrder() {
    Mask8 mask(40, 30);
    mask.rect(14, 2, 22, 6);    // numerator, component 0
    mask.rect(32, 2, 36, 4);    // unrelated small region, component 1
    mask.rect(6, 12, 30, 13);   // fraction bar, component 2
    mask.rect(14, 20, 22, 24);  // denominator, component 3
    const int bar[] = {2};
    const int top[] = {0, 1};
    const int bottom[] = {3};
    const we::PlanUnitView planned[] = {{bar, 1, we::PlanKind::Rule}, {top, 2, we::PlanKind::Glyph}, {bottom, 1, we::PlanKind::Glyph}};
    we::UnitRequest request;
    request.mask = mask.view();
    request.mode = we::UnitMode::ComponentPerUnit;
    request.plan = {mask.width, mask.height, 4, planned, 3};
    const auto layout = we::buildLayout({request});
    check(layout.units.size() == 3, "One unit per plan unit");
    // Bar first, and its ink is the wide thin rule.
    check(layout.units[0].components.size() == 1, "The bar is one component");
    checkNear(layout.units[0].components[0].inkMinY, 12.0, 1e-4, "The plan must draw the bar first");
    // Then the numerator glyph together with the unrelated region: one stagger step.
    check(layout.units[1].components.size() == 2, "A plan unit may group several ink regions");
    checkNear(layout.units[1].components[0].inkMinY, 2.0, 1e-4, "The numerator follows the bar");
    checkNear(layout.units[1].components[1].inkMinX, 32.0, 1e-4, "The grouped region is drawn with its unit");
    checkNear(layout.units[2].components[0].inkMinY, 20.0, 1e-4, "The denominator comes last");
    check(layout.totalLength > 0.f, "A planned layout must keep its stroke length");
    checkOwnership(layout, mask, 0.f, 0.f, "A planned layout must still tile every texel exactly once");
    check(drawnInkTexels(layout, mask) == maskInkTexels(mask), "A planned layout must not drop or duplicate ink");

    // Without the plan the same mask keeps the reading order: top band first.
    const auto fallback = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::ComponentPerUnit}});
    check(fallback.units.size() == 4, "Without a plan every region is its own unit");
    checkNear(fallback.units[0].components[0].inkMinY, 2.0, 1e-4, "The reading order starts at the top band");
}

// A stale or inconsistent plan must never be used: the reading order stays.
void testWritingPlanFallsBackWhenItDoesNotMatch() {
    Mask8 mask(40, 30);
    mask.rect(14, 2, 22, 6);
    mask.rect(6, 12, 30, 13);
    mask.rect(14, 20, 22, 24);
    const int all[] = {2, 0, 1};
    const we::PlanUnitView planned[] = {{all, 3, we::PlanKind::Glyph}};
    auto layoutWith = [&](we::PlanView plan) {
        we::UnitRequest request;
        request.mask = mask.view();
        request.mode = we::UnitMode::ComponentPerUnit;
        request.plan = plan;
        return we::buildLayout({request});
    };
    check(layoutWith({mask.width, mask.height, 3, planned, 1}).units.size() == 1, "A matching plan is used");
    check(layoutWith({mask.width + 1, mask.height, 3, planned, 1}).units.size() == 3, "A plan for another size is ignored");
    check(layoutWith({mask.width, mask.height, 4, planned, 1}).units.size() == 3, "A plan with the wrong component count is ignored");
    const int repeated[] = {0, 0, 1};
    const we::PlanUnitView duplicated[] = {{repeated, 3, we::PlanKind::Glyph}};
    check(layoutWith({mask.width, mask.height, 3, duplicated, 1}).units.size() == 3, "A plan that repeats a component is ignored");
    const int missing[] = {0, 1};
    const we::PlanUnitView incomplete[] = {{missing, 2, we::PlanKind::Glyph}};
    check(layoutWith({mask.width, mask.height, 3, incomplete, 1}).units.size() == 3, "A plan that drops a component is ignored");
    const int outOfRange[] = {0, 1, 7};
    const we::PlanUnitView beyond[] = {{outOfRange, 3, we::PlanKind::Glyph}};
    check(layoutWith({mask.width, mask.height, 3, beyond, 1}).units.size() == 3, "A plan with an out-of-range component is ignored");
}

// The checked-in plan of a built-in formula must describe the checked-in mask.
void testBuiltinWritingPlan() {
    const auto& entry = builtin("flight");
    Mask8 mask = decodeBuiltin(entry);
    const auto& stored = builtinPlan("flight");
    check(stored.width == entry.width && stored.height == entry.height, "The plan must belong to the same mask size");
    // Without a plan every ink region is its own unit, so that layout counts the
    // mask's components exactly; the stored plan has to name the same number.
    const auto components = we::buildLayout({{mask.view(), 0.f, 0.f, we::UnitMode::ComponentPerUnit}});
    check(components.units.size() == static_cast<std::size_t>(stored.componentCount),
        "The plan must name exactly the mask's ink components");
    std::vector<we::PlanUnitView> storage;
    const we::PlanView plan = planView(stored, storage);
    we::UnitRequest request;
    request.mask = mask.view();
    request.mode = we::UnitMode::ComponentPerUnit;
    request.plan = plan;
    const auto layout = we::buildLayout({request});
    check(layout.units.size() == static_cast<std::size_t>(stored.unitCount), "Every plan unit becomes one staggered step");
    // Exercise the exact lookup used by the live renderer, not only the raw table.
    const auto* installed = physics_editor::formula_plan::find("flight");
    check(installed != nullptr, "The live renderer must find the built-in flight plan");
    request.plan = *installed;
    const auto liveLayout = we::buildLayout({request});
    check(liveLayout.units.size() == layout.units.size(), "Live lookup must use structural units, not the fallback");
    for (std::size_t i = 0; i < layout.units.size(); ++i) {
        check(liveLayout.units[i].components.size() == layout.units[i].components.size(), "Live and generated plan grouping must agree");
        checkNear(liveLayout.units[i].components[0].inkMinX, layout.units[i].components[0].inkMinX, 0.001, "Live and generated plan order must agree");
    }
    checkOwnership(layout, mask, 0.f, 0.f, "The built-in plan must tile its mask exactly once");
    check(drawnInkTexels(layout, mask) == maskInkTexels(mask), "The built-in plan must not drop or duplicate ink");

    auto inkBounds = [](const we::Unit& unit) {
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (const auto& component : unit.components) {
            minX = std::min(minX, component.inkMinX); maxX = std::max(maxX, component.inkMaxX);
            minY = std::min(minY, component.inkMinY); maxY = std::max(maxY, component.inkMaxY);
        }
        return std::array<float, 4>{minX, minY, maxX, maxY};
    };
    // T comes first and `=` second: the outer row is written left to right.
    const auto first = inkBounds(layout.units[0]);
    for (const auto& unit : layout.units) check(first[0] <= inkBounds(unit)[0] + 0.5f, "flight must start at the leftmost glyph (T)");
    const auto second = inkBounds(layout.units[1]);
    check(second[0] > first[2], "`=` follows T");
    check(layout.units[1].components.size() == 2, "The two bars of `=` form one unit");
    // The fraction bar is a rule and precedes every unit above or below it.
    check(stored.units[2].kind == 1, "flight's third unit must be the fraction bar");
    const auto bar = inkBounds(layout.units[2]);
    check(bar[3] - bar[1] <= 6.f, "A fraction bar is a thin rule");
    for (std::size_t index = 0; index < layout.units.size(); ++index) {
        if (index == 2) continue;
        const auto box = inkBounds(layout.units[index]);
        if (box[2] <= bar[0] + 1.f || box[0] >= bar[2] - 1.f) continue;  // outside the bar's span
        const bool above = box[3] <= bar[1] + 1.f, below = box[1] >= bar[3] - 1.f;
        if (!above && !below) continue;
        check(2 < index, "The fraction bar must be drawn before its numerator and denominator");
    }
    // The denominator is the last unit and sits below the bar.
    const auto last = inkBounds(layout.units.back());
    check(last[1] > bar[3], "The denominator closes the fraction");
}

Json writeSpec(Json patch) {
    Json spec = {{"start", 1.0}, {"duration", 2.0}, {"stagger", 0.5}, {"phase", 0.5},
        {"outlineWidth", 1.5}, {"outline", true}, {"fill", true}, {"easing", "smooth"}};
    spec.update(patch);
    return spec;
}

void testCatalog() {
    // get_catalog feeds MCP clients: it must stay one object. A mis-nested braced
    // list once turned it into an array and every catalog["..."] lookup failed.
    const Document document;
    const Json catalog = document.catalog();
    check(catalog.is_object(), "get_catalog must return a JSON object");
    check(catalog.contains("documentVersion") && catalog["documentVersion"] == 1, "The catalog must carry its version");
    check(catalog.contains("primitives") && catalog["primitives"].is_array(), "The catalog must list primitives");
    check(catalog["primitives"].size() == 11, "The catalog must keep the existing eleven primitives");
    check(catalog.contains("commands") && catalog["commands"].is_array(), "The catalog must list commands");
    bool hasSetWrite = false;
    for (const auto& command : catalog["commands"]) {
        check(command.is_object() && command.contains("op") && command.contains("description"), "A command entry must be an object");
        if (command["op"] == "set_write") hasSetWrite = true;
    }
    check(hasSetWrite, "The catalog must document set_write");
    check(catalog.contains("write") && catalog["write"].is_object(), "The catalog must document the write entry");
    const auto& write = catalog["write"];
    check(write.contains("appliesTo") && write["appliesTo"].is_string(), "write.appliesTo must be a string");
    check(write["appliesTo"].get<std::string>().find("text") != std::string::npos, "write must name its object types");
    check(write.contains("default") && write["default"].is_object(), "write.default must be an object");
    const auto& defaults = write["default"];
    for (const char* key : {"start", "duration", "stagger", "phase", "outlineWidth", "outline", "fill", "easing"})
        check(defaults.contains(key), "write.default must document every field");
    check(defaults["phase"].get<double>() > 0.05 && defaults["phase"].get<double>() < 0.95,
        "The documented default phase must leave room for the fill");
    check(write.contains("note") && write["note"].is_string(), "write must explain its limits");
}

void testTimingBoundaries() {
    Json object = {{"type", "text"}, {"properties", {{"text", "abc"}}}, {"write", writeSpec(Json::object())}};
    const std::size_t units = 3;
    auto at = [&](double time, Json spec = Json()) {
        Json target = object;
        if (!spec.is_null()) target["write"] = spec;
        return we::evaluate(target, time, units);
    };
    // Unit windows: [1.0,2.0], [1.5,2.5], [2.0,3.0]; unit length 1.0 s.
    const auto before = at(0.5);
    check(before.total == 0.f, "Before start the progress is zero");
    for (const auto& unit : before.units) check(unit.stroke == 0.f && unit.fill == 0.f, "Nothing may be drawn before start");
    const auto atStart = at(1.0);
    check(atStart.total == 0.f, "Exactly at start nothing is visible yet");
    check(atStart.units[0].stroke == 0.f && atStart.units[0].fill == 0.f, "Zero progress draws no glyph");
    const auto middle = at(1.75);
    checkNear(middle.total, 0.375, 1e-6, "Total progress is linear in time");
    // smoothstep(0.75) = 0.84375 -> stroke clamps to 1, fill = (0.84375-0.5)/0.5.
    checkNear(middle.units[0].stroke, 1.0, 1e-6, "First unit finished its outline");
    checkNear(middle.units[0].fill, 0.6875, 1e-5, "First unit is filling");
    // smoothstep(0.25) = 0.15625 -> stroke = 0.3125, fill not started.
    checkNear(middle.units[1].stroke, 0.3125, 1e-5, "Second unit is staggered behind the first");
    check(middle.units[1].fill == 0.f, "Second unit has not started filling");
    check(middle.units[2].stroke == 0.f && middle.units[2].fill == 0.f, "Third unit has not started");
    const auto complete = at(3.0);
    check(complete.total == 1.f, "The animation completes exactly at start + duration");
    checkNear(complete.units[2].fill, 1.0, 1e-6, "The last unit ends exactly at the object window");
    const auto after = at(9.0);
    check(after.total == 1.f, "Progress clamps to one after the window");
    checkNear(after.units[0].fill, 1.0, 1e-6, "Finished units stay filled");
    // Linear easing keeps the raw fraction.
    const auto linear = at(1.75, writeSpec({{"easing", "linear"}}));
    checkNear(linear.units[0].fill, 0.5, 1e-6, "Linear easing must not reshape the fraction");
    checkNear(linear.units[1].stroke, 0.5, 1e-6, "Linear easing must not reshape the stroke");
    // A single unit consumes the whole window without stagger.
    const auto single = we::evaluate(object, 2.0, 1);
    checkNear(single.units[0].stroke, 1.0, 1e-5, "One unit finishes its outline at the phase split");
    check(single.total == 0.5f, "Total progress is independent of the unit count");
    // Fill only / outline only.
    const auto fillOnly = at(1.5, writeSpec({{"outline", false}}));
    check(fillOnly.outline == false && fillOnly.fill == true, "Phase switches must be reported");
    checkNear(fillOnly.units[0].fill, 0.5, 1e-5, "Fill-only fades the fill over the unit window");
    check(fillOnly.units[0].stroke == 1.f, "Fill-only has no partial outline to draw");
    const auto outlineOnly = at(1.5, writeSpec({{"fill", false}}));
    checkNear(outlineOnly.units[0].stroke, 0.5, 1e-5, "Outline-only draws over the unit window");
    check(outlineOnly.units[0].fill == 0.f, "Outline-only never fills");
    // Objects without a write entry are always complete: the renderer falls back
    // to the normal glyph path and old projects are unchanged.
    Json plain = {{"type", "text"}, {"properties", {{"text", "abc"}}}};
    const auto untouched = we::evaluate(plain, 0.0, units);
    check(untouched.total == 1.f, "A document without write must draw the plain glyph at any time");
    check(untouched.units.empty(), "A document without write has no unit phases");

    // Just after the start nothing may be drawn yet: a stroke that is a rounding
    // error long, or a fill that starts early, would flash the glyph.
    const auto justAfterStart = at(1.0 + 1e-6);
    for (const auto& unit : justAfterStart.units) check(unit.fill == 0.f, "The fill must not start with the window");
    check(justAfterStart.units[0].stroke < 1e-4f, "The outline must start empty");
    // ...and just before the end the animation must not be finished already.
    const auto justBeforeEnd = at(3.0 - 1e-3);
    check(justBeforeEnd.total < 1.f, "The object window must not complete early");
    check(justBeforeEnd.units[2].fill < 1.f, "The fill must not complete before the window ends");
    const auto atEnd = at(3.0);
    checkNear(atEnd.units[2].fill, 1.0, 1e-5, "The fill completes exactly at the end of the window");

    // phase = 1 used to leave the fill fully visible from the first frame; the
    // bound keeps the fill inside the unit window instead.
    const auto latePhase = at(1.5, writeSpec({{"phase", 0.95}}));
    check(latePhase.units[0].fill == 0.f, "A late fill must still be empty halfway through the outline");
    checkNear(latePhase.units[0].stroke, 0.5 / 0.95, 1e-4, "The outline uses the whole phase share");
    checkNear(at(2.0, writeSpec({{"phase", 0.95}})).units[0].fill, 1.0, 1e-4, "The fill still completes at the end");
    check(we::evaluate(object, 1.0, units).units[0].fill == 0.f, "The fill must stay hidden at the very start");
}

void testPureTimeEvaluation() {
    Json object = {{"type", "formula"}, {"properties", {{"formula", "flight"}}}, {"write", writeSpec({{"stagger", 0.6}})}};
    // Seeking is a pure function of time: forward, backward and repeated
    // evaluation must agree, so playback and export never depend on frame history.
    const auto forward = we::evaluate(object, 2.4, 6);
    (void)we::evaluate(object, 9.9, 6);
    (void)we::evaluate(object, 0.1, 6);
    const auto again = we::evaluate(object, 2.4, 6);
    check(forward.total == again.total, "Repeated evaluation must be identical");
    for (std::size_t i = 0; i < forward.units.size(); ++i) {
        check(forward.units[i].stroke == again.units[i].stroke, "Stroke must not depend on earlier frames");
        check(forward.units[i].fill == again.units[i].fill, "Fill must not depend on earlier frames");
    }
    std::vector<float> lastStroke, lastFill;
    for (int step = 0; step <= 200; ++step) {
        const auto progress = we::evaluate(object, 3.0 * step / 200.0, 6);
        if (lastStroke.empty()) {
            lastStroke.assign(progress.units.size(), -1.f);
            lastFill.assign(progress.units.size(), -1.f);
        }
        for (std::size_t i = 0; i < progress.units.size(); ++i) {
            check(progress.units[i].stroke >= lastStroke[i] - 1e-6f, "Stroke progress must not go backwards in time");
            check(progress.units[i].fill >= lastFill[i] - 1e-6f, "Fill progress must not go backwards in time");
            lastStroke[i] = progress.units[i].stroke;
            lastFill[i] = progress.units[i].fill;
        }
    }
}

void testDocumentContract() {
    Document document;
    const Json spec = writeSpec({{"duration", 1.5}});
    const auto baseline = document.snapshot();
    const auto apply = [&](Json write, const char* id = "title") {
        return Json{{"method", "apply"}, {"args", {{"commands", Json::array({{{"op", "set_write"}, {"id", id}, {"write", std::move(write)}}})}}}};
    };
    document.dispatch(apply(spec));
    check(we::hasWrite(document.object("title")), "set_write must store the entry animation");
    check(document.object("title")["write"]["duration"] == 1.5, "set_write must not rewrite the spec");
    check(document.history()["entries"].back()["status"] == "success", "set_write must be journalled");
    document.dispatch({{"method", "undo"}});
    check(!we::hasWrite(document.object("title")) && document.scene() == baseline["document"], "Undo must remove the entry animation");
    document.dispatch({{"method", "redo"}});
    check(we::hasWrite(document.object("title")), "Redo must restore the entry animation");

    // The same command path is shared by the UI and MCP; every rejection must keep
    // the document untouched.
    const auto before = document.snapshot();
    rejects([&] { document.dispatch(apply(spec, "ball")); }, "Write on a non text/formula object must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"duration", 99.0}}))); }, "A window past the scene duration must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"start", -1.0}}))); }, "A negative start must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"easing", "bounce"}}))); }, "An unknown easing must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"outline", "yes"}}))); }, "A non-boolean phase switch must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"outline", false}, {"fill", false}}))); }, "Both phases off must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"mystery", 1}}))); }, "An unknown write field must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"stagger", 4.0}}))); }, "Stagger outside [0,1] must be rejected");
    rejects([&] { document.dispatch(apply(writeSpec({{"phase", 1.0}}))); }, "phase = 1 must be rejected (the fill needs time left)");
    rejects([&] { document.dispatch(apply(writeSpec({{"phase", 0.0}}))); }, "phase = 0 must be rejected");
    check(document.snapshot() == before, "A rejected write edit must not change the document");

    // Removal through merge_patch (null) and through set_write(null).
    document.dispatch({{"method", "apply"}, {"args", {{"commands", Json::array({{{"op", "update"}, {"id", "title"}, {"patch", {{"write", nullptr}}}}})}}}});
    check(!we::hasWrite(document.object("title")), "update with a null write must remove the field");
    document.dispatch(apply(spec));
    document.dispatch(apply(Json()));
    check(!we::hasWrite(document.object("title")), "set_write(null) must remove the field");

    // Component definitions stay static: children may not carry an entry
    // animation, exactly like they may not carry motion or animation channels.
    Json definition = components::blank("write_child");
    definition["objects"].push_back({{"id", "label"}, {"type", "text"}, {"name", "标签"},
        {"properties", {{"x", 0.0}, {"y", 0.0}, {"text", "中文"}, {"size", 16.0}}},
        {"visible", true}, {"locked", false}, {"step", 0}, {"write", spec}});
    rejects([&] { Document draft(Registry{}, definition); }, "A definition child must not carry a write entry");

    // create carries the entry animation, and a new object of the wrong type is
    // rejected by the same validation as every other write edit.
    document.dispatch({{"method", "apply"}, {"args", {{"commands", Json::array({{{"op", "create"}, {"id", "fresh"}, {"type", "text"},
        {"properties", {{"x", 0.0}, {"y", 0.0}, {"text", "手写"}, {"size", 18.0}}}, {"write", spec}}})}}}});
    check(we::hasWrite(document.object("fresh")), "create must carry a write entry");
    rejects([&] {
        document.dispatch({{"method", "apply"}, {"args", {{"commands", Json::array({{{"op", "create"}, {"id", "bad"}, {"type", "point"}, {"write", spec}}})}}}});
    }, "A write entry on a new non text/formula object must be rejected");
    document.dispatch({{"method", "apply"}, {"args", {{"commands", Json::array({{{"op", "delete"}, {"id", "fresh"}}})}}}});

    // Serialization: the field survives a save/load round trip.
    const auto path = std::filesystem::current_path() / std::filesystem::u8path("write-entry-roundtrip.json");
    document.dispatch(apply(spec));
    document.dispatch({{"method", "save"}, {"args", {{"path", path.u8string()}, {"overwrite", true}}}});
    Document loaded;
    loaded.dispatch({{"method", "load"}, {"args", {{"path", path.u8string()}}}});
    check(loaded.scene() == document.scene(), "A saved scene must keep its write entries");
    check(we::hasWrite(loaded.object("title")), "The loaded object must still animate its entry");
    std::filesystem::remove(path);
}

void testDemoScene() {
    const Json demo = we::demo();
    Document document(Registry{}, demo);
    check(document.scene() == demo, "The demo scene must pass document validation");
    int texts = 0, formulas = 0, animated = 0;
    for (const auto& object : demo["objects"]) {
        if (!we::hasWrite(object)) continue;
        ++animated;
        if (object["type"] == "text") {
            ++texts;
            const std::string text = object["properties"]["text"].get<std::string>();
            check(text.size() > 6, "The demo must contain real Chinese text");
        } else if (object["type"] == "formula") {
            ++formulas;
            const std::string name = object["properties"]["formula"].get<std::string>();
            bool known = false;
            for (const auto& entry : formula_cache::entries) if (name == entry.name) known = true;
            check(known, "Every demo formula must be a built-in resource");
        }
    }
    check(texts >= 1 && formulas >= 1, "The demo must show both Chinese text and formulas");
    check(animated >= 3, "The demo must animate several entries");
    // The demo is authored for a readable 1280x720 fit export: large text and
    // formulas in a compact stack whose windows all fit the scene duration.
    double top = -1e9, bottom = 1e9;
    for (const auto& object : demo["objects"]) {
        const auto& properties = object["properties"];
        if (object["type"] == "text") check(properties["size"].get<double>() >= 36.0, "Demo text must be readable at 1:1");
        if (object["type"] == "formula") check(properties["scale"].get<double>() >= 2.0, "Demo formulas must be readable at 1:1");
        top = std::max(top, properties["y"].get<double>());
        bottom = std::min(bottom, properties["y"].get<double>());
        if (!we::hasWrite(object)) continue;
        const auto& write = object["write"];
        const double start = write["start"].get<double>(), length = write["duration"].get<double>();
        check(start >= 0.0 && start + length <= demo["duration"].get<double>() + 1e-9,
            "Every demo window must stay inside the scene duration");
        check(write["phase"].get<double>() > 0.0 && write["phase"].get<double>() < 1.0,
            "Every demo window must leave time for the fill");
    }
    check(top - bottom <= 12.0, "The demo stack must stay compact for a 1280x720 fit export");
    // The checked-in demo project must stay identical to the generated scene, so
    // the reproducible file can never drift away from the code.
    const auto path = std::filesystem::current_path() / "scenes" / "write-entry.json";
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Missing " + path.u8string() + "; regenerate it with: physics_write_entry_test --emit-demo scenes/write-entry.json");
    Json stored;
    stream >> stored;
    check(!stored.is_discarded(), "scenes/write-entry.json must be valid JSON");
    if (stored != demo) throw std::runtime_error("scenes/write-entry.json is stale; regenerate it with: physics_write_entry_test --emit-demo scenes/write-entry.json");
}

void testCjkChain() {
    const auto& fonts = graphics::font_support::systemFontCandidates();
    if (fonts.empty()) {
        std::cerr << "[write entry] no system font found; skipping the CJK outline chain\n";
        return;
    }
    const std::string text = u8"中文公式";
    const auto glyphs = graphics::font_support::rasterizeText(text, 64.0f, fonts);
    check(glyphs.size() == 4, "One raster entry per decoded character");
    int bitmaps = 0;
    for (const auto& glyph : glyphs) if (glyph.hasBitmap) ++bitmaps;
    check(bitmaps == 4, "Every CJK character must rasterize an ink bitmap");
    std::vector<we::UnitRequest> requests;
    requests.reserve(glyphs.size());
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        check(glyphs[i].bytes > 0, "Each raster must address its character in the source text");
        if (i > 0) check(glyphs[i].penX > glyphs[i - 1].penX, "The pen must advance in text order");
        we::UnitRequest request;
        request.x = glyphs[i].penX + glyphs[i].x;
        request.y = glyphs[i].penY + glyphs[i].y;
        request.mode = we::UnitMode::SingleUnit;
        request.mask = {glyphs[i].hasBitmap ? glyphs[i].alpha.data() : nullptr, glyphs[i].width, glyphs[i].height};
        requests.push_back(request);
    }
    const auto layout = we::buildLayout(requests);
    check(layout.units.size() == 4, "One animation unit per CJK character");
    check(glyphs.back().penX > 0.f, "The pen must advance between CJK characters");
    for (std::size_t i = 0; i < layout.units.size(); ++i) {
        check(!layout.units[i].components.empty(), "Every CJK character must produce ink regions");
        check(layout.units[i].strokeLength > 5.f, "A CJK outline must have a real length");
        // The traced outline must stay inside the character's own bitmap: a stray
        // point would mean the tracer bridged two characters.
        const float left = requests[i].x - 1.5f, right = requests[i].x + static_cast<float>(requests[i].mask.width) + 1.5f;
        const float top = requests[i].y - 1.5f, bottom = requests[i].y + static_cast<float>(requests[i].mask.height) + 1.5f;
        for (const auto& component : layout.units[i].components) {
            check(component.minX >= left && component.maxX <= right, "CJK outlines must stay inside their own character cell");
            for (const auto& loop : component.loops) {
                // The contours must be placed with the character: a loop left at the
                // mask origin would stack every stroke on the first character.
                for (const auto& point : loop.points) {
                    check(point.x >= left && point.x <= right, "A CJK outline must be placed with its character (x)");
                    check(point.y >= top && point.y <= bottom, "A CJK outline must be placed with its character (y)");
                }
            }
        }
        // Chinese glyphs are not a single blob: the raster order inside one
        // character is a documented simplification, not the real stroke order.
        for (const auto& component : layout.units[i].components) check(!component.loops.empty(), "Every ink region needs an outline");
    }
    std::size_t loops = 0;
    for (const auto& unit : layout.units) for (const auto& component : unit.components) loops += component.loops.size();
    check(loops >= 4, "CJK characters must expose several contours");

    // A newline starts a new line without advancing the pen horizontally.
    const auto lines = graphics::font_support::rasterizeText(u8"中\n文", 64.0f, fonts);
    check(lines.size() == 3, "The newline must stay one animation step");
    check(lines[2].penX >= 0.f && lines[2].penX < lines[0].penX + 1.f, "A newline returns the pen to the line origin");
    check(lines[2].penY > lines[0].penY, "A newline advances the pen to the next line");
}

void testUtf8Offsets() {
    // rasterizeText() and decodeUtf8() must agree, malformed input included.
    const std::string samples[] = {u8"abc", u8"中文", u8"a中b", u8"é", "\xC3", "\xFF\xFE", u8"a\r\nb", u8"αβ₁₂"};
    for (const std::string& sample : samples) {
        const auto plain = graphics::font_support::decodeUtf8(sample);
        const auto detailed = graphics::font_support::decodeUtf8Characters(sample);
        check(plain.size() == detailed.size(), "decodeUtf8 and decodeUtf8Characters must agree");
        for (std::size_t i = 0; i < plain.size(); ++i) {
            check(plain[i] == detailed[i].codepoint, "decodeUtf8 and decodeUtf8Characters must agree");
            check(detailed[i].offset + detailed[i].bytes <= sample.size(), "A decoded character must stay inside the text");
            const auto single = graphics::font_support::decodeUtf8(sample.substr(detailed[i].offset, detailed[i].bytes));
            check(single.size() == 1 && single[0] == detailed[i].codepoint, "A character range must decode back to its code point");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--emit-demo") {
            const std::filesystem::path path = std::filesystem::u8path(argv[2]);
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("Cannot write " + path.u8string());
            stream << we::demo().dump(2) << '\n';
            std::cerr << "wrote " << path.u8string() << '\n';
            return 0;
        }
        testSubPixelOutline();
        testMaskOffset();
        testFringeOwnership();
        testDisjointContoursAreNotBridged();
        testDiagonalPixelsStaySeparate();
        testHolesAreExcludedFromTheFill();
        testFillCoversTheFilterSupport();
        testFaintInkIsNeverDropped();
        testBorderAndHoleBoundaries();
        testUnitOrder();
        testWritingPlanOrder();
        testWritingPlanFallsBackWhenItDoesNotMatch();
        testBuiltinWritingPlan();
        testCatalog();
        testTimingBoundaries();
        testPureTimeEvaluation();
        testDocumentContract();
        testUtf8Offsets();
        testCjkChain();
        testDemoScene();
        std::cout << "Write entry tests passed: sub-pixel outlines, mask placement, fringe ownership, disjoint contours, holes, "
                     "filter support, faint ink, border and hole boundaries, "
                     "unit order, writing plan order and fallback, built-in flight plan, catalog contract, timing boundaries, "
                     "pure time evaluation, document contract, serialization, "
                     "CJK chain, demo scene\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
