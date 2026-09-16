#pragma once
// Storage and validation for formula writing plans.
//
// A plan is optional metadata beside a formula mask: an ordered list of units,
// each one naming the ink components (raster-scan order) that form one staggered
// step of the Write entry animation. `tools/formula_writing_plan.py` generates it
// from the same PDF the mask was rendered from. Runtime formulas carry theirs in
// the project's resource library; the built-in table is generated into
// assets/formulas/formula_writing_plan.hpp and resolved by find().
//
// Nothing here trusts the stored data: a plan is parsed with hard limits and a
// full component-coverage check, and `write_entry::buildLayout` uses it only when
// it matches the mask exactly. A formula without a plan, or with an unusable one,
// keeps the previous reading-order animation.
#include "document.hpp"
#include "write_entry.hpp"

#include <string>
#include <vector>

namespace physics_editor::formula_plan {

// Parsed plan. `components` holds every unit's payload back to back and `units`
// points into it, so the view stays valid as long as the plan is not rebuilt.
struct Plan {
    std::string name;
    int width = 0, height = 0;
    int componentCount = 0;
    std::vector<int> components;               // every unit's component ids, back to back
    std::vector<int> unitOffsets, unitCounts;  // where each unit starts / how long it is
    std::vector<write_entry::PlanKind> unitKinds;
    std::vector<write_entry::PlanUnitView> units;
    write_entry::PlanView view;

    void finish();  // rebuild `units`/`view` from `components`
};

// Replaces the runtime plan library (one entry per custom formula asset, in the
// same order as the masks handed to math_text::setCustom).
void setInstalled(std::vector<Plan> plans);

// Finds a runtime plan first, then the generated built-in plan, or nullptr.
const write_entry::PlanView* find(const char* name);

// Parses and validates one stored writingPlan object. Throws on any unknown
// field, out-of-range value, duplicate component or uncovered component.
Plan parse(const Json& value);

} // namespace physics_editor::formula_plan
