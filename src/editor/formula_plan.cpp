#include "formula_plan.hpp"
#include "formulas/formula_writing_plan.hpp"

#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace physics_editor::formula_plan {
namespace {

constexpr int kMaxUnits = 512;
constexpr int kMaxComponents = 8192;
constexpr int kMaxWidth = 2048;
constexpr int kMaxHeight = 1024;
constexpr float kMaxBoxCoordinate = 4096.f;

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

bool integral(const Json& value) { return value.is_number_integer() || value.is_number_unsigned(); }

int checkedInteger(const Json& value, int low, int high, const char* message) {
    require(integral(value), message);
    const int64_t number = value.get<int64_t>();
    require(number >= low && number <= high, message);
    return static_cast<int>(number);
}

std::vector<Plan>& installedPlans() {
    static std::vector<Plan> plans;
    return plans;
}

const std::vector<Plan>& builtinPlans() {
    static const std::vector<Plan> plans = [] {
        std::vector<Plan> result;
        result.reserve(std::size(formula_writing_plan::entries));
        for (const auto& entry : formula_writing_plan::entries) {
            Plan plan;
            plan.name = entry.name;
            plan.width = entry.width;
            plan.height = entry.height;
            plan.componentCount = entry.componentCount;
            for (int i = 0; i < entry.unitCount; ++i) {
                const auto& unit = entry.units[i];
                plan.unitOffsets.push_back(static_cast<int>(plan.components.size()));
                plan.unitCounts.push_back(unit.count);
                plan.unitKinds.push_back(unit.kind == 1 ? write_entry::PlanKind::Rule : write_entry::PlanKind::Glyph);
                plan.components.insert(plan.components.end(), unit.components, unit.components + unit.count);
            }
            plan.finish();
            result.push_back(std::move(plan));
        }
        return result;
    }();
    return plans;
}

} // namespace

void Plan::finish() {
    units.clear();
    units.reserve(unitOffsets.size());
    for (std::size_t index = 0; index < unitOffsets.size(); ++index) {
        units.push_back({components.data() + unitOffsets[index], unitCounts[index], unitKinds[index]});
    }
    view = {width, height, componentCount, units.data(), static_cast<int>(units.size())};
}

void setInstalled(std::vector<Plan> plans) { installedPlans() = std::move(plans); }

const write_entry::PlanView* find(const char* name) {
    if (name == nullptr) return nullptr;
    for (const Plan& plan : installedPlans()) if (plan.name == name) return &plan.view;
    for (const Plan& plan : builtinPlans()) if (plan.name == name) return &plan.view;
    return nullptr;
}

Plan parse(const Json& value) {
    require(value.is_object(), "writingPlan must be an object");
    static const std::set<std::string> fields{"version", "width", "height", "componentCount", "units"};
    for (auto it = value.begin(); it != value.end(); ++it) require(fields.count(it.key()) != 0, "Unknown writingPlan field: " + it.key());
    require(value.contains("version") && value.at("version") == 1, "Unsupported writingPlan version");
    require(value.contains("width") && value.contains("height") && value.contains("componentCount") && value.contains("units"),
        "writingPlan is incomplete");
    Plan plan;
    plan.width = checkedInteger(value.at("width"), 1, kMaxWidth, "Invalid writingPlan width");
    plan.height = checkedInteger(value.at("height"), 1, kMaxHeight, "Invalid writingPlan height");
    plan.componentCount = checkedInteger(value.at("componentCount"), 1, kMaxComponents, "Invalid writingPlan component count");
    const Json& stored = value.at("units");
    require(stored.is_array() && !stored.empty() && stored.size() <= static_cast<std::size_t>(kMaxUnits),
        "writingPlan must list 1..512 units");
    static const std::set<std::string> unitFields{"kind", "box", "text", "code", "components"};
    std::vector<char> seen(static_cast<std::size_t>(plan.componentCount), 0);
    for (const auto& unit : stored) {
        require(unit.is_object(), "A writingPlan unit must be an object");
        for (auto it = unit.begin(); it != unit.end(); ++it) require(unitFields.count(it.key()) != 0, "Unknown writingPlan unit field: " + it.key());
        write_entry::PlanKind kind = write_entry::PlanKind::Glyph;
        if (unit.contains("kind")) {
            require(unit.at("kind").is_string(), "writingPlan unit kind must be a string");
            const std::string name = unit.at("kind").get<std::string>();
            require(name == "glyph" || name == "rule", "writingPlan unit kind must be glyph or rule");
            kind = name == "rule" ? write_entry::PlanKind::Rule : write_entry::PlanKind::Glyph;
        }
        if (unit.contains("box")) {
            const Json& box = unit.at("box");
            require(box.is_array() && box.size() == 4, "writingPlan unit box must have four numbers");
            for (const auto& coordinate : box) {
                require(coordinate.is_number(), "writingPlan unit box must have four numbers");
                const double number = coordinate.get<double>();
                require(std::isfinite(number) && std::abs(number) <= kMaxBoxCoordinate, "writingPlan unit box is out of range");
            }
        }
        if (unit.contains("text")) require(unit.at("text").is_string() && unit.at("text").get_ref<const std::string&>().size() <= 8,
            "writingPlan unit text is too long");
        if (unit.contains("code")) checkedInteger(unit.at("code"), -1, 0x10FFFF, "Invalid writingPlan unit code");
        require(unit.contains("components") && unit.at("components").is_array(), "A writingPlan unit needs its components");
        const Json& list = unit.at("components");
        require(!list.empty() && list.size() <= static_cast<std::size_t>(plan.componentCount), "Invalid writingPlan component list");
        plan.unitOffsets.push_back(static_cast<int>(plan.components.size()));
        for (const auto& entry : list) {
            const int index = checkedInteger(entry, 0, plan.componentCount - 1, "writingPlan component index out of range");
            require(!seen[static_cast<std::size_t>(index)], "writingPlan uses a component twice");
            seen[static_cast<std::size_t>(index)] = 1;
            plan.components.push_back(index);
        }
        plan.unitCounts.push_back(static_cast<int>(list.size()));
        plan.unitKinds.push_back(kind);
    }
    require(plan.components.size() == static_cast<std::size_t>(plan.componentCount),
        "writingPlan must name every component exactly once");
    plan.finish();
    return plan;
}

} // namespace physics_editor::formula_plan
