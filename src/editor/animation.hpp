#pragma once
#include "document.hpp"

namespace physics_editor::animation {
// Channels are absolute authored values, not forces integrated by a simulator.
Json evaluate(const Json& animation,double time,double duration);
void validate(const Json& object,double duration,const Registry& registry);
Json properties(const Json& object,double time,double duration);
Json demo();
}
