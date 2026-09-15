#pragma once
// Write-style entry drawing for the editor canvas.
//
// Geometry is cached per text / per formula resource and only rebuilt when the
// text or the resource library changes; a frame evaluates the animation as a
// pure function of the document time, so playback, arbitrary seeks and the
// offscreen export all draw the same picture. See src/editor/write_entry.hpp for
// the timing model.
#include "document.hpp"
#include "imgui.h"

#include <cstdint>

namespace physics_editor::write_draw {

// Draws the Write entry animation of one text object. Returns true when the call
// was handled: either the animation is still running (part of it was drawn, or
// nothing must be visible yet), or nothing at all may be drawn. Returns false
// when the caller has to draw the plain text — no `write` entry, or the entry
// has finished, which keeps the last frame pixel-identical to the previous
// behaviour.
bool text(ImDrawList* list, const Json& object, ImVec2 position, float pixelSize, const char* text,
    ImU32 tint, double time, float canvasScale);

// Same contract for a formula object. `resources` invalidates the cached mask
// geometry when the workspace resource library is replaced.
bool formula(ImDrawList* list, const Json& object, ImVec2 position, ImU32 tint, float scale,
    double time, float canvasScale, uint64_t resources);
}
