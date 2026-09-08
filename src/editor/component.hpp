#pragma once
#include "json.hpp"
#include <string>

namespace physics_editor {
class Registry;
struct State;
namespace components {
using Json=nlohmann::json;
// A deliberately bounded arithmetic language; no scripting engine or file I/O.
double expression(const std::string& source,const Json& variables);
void validateDefinition(const Json& definition,const Registry& registry);
Json materialize(const Json& definition,const Json& overrides=Json::object());
Json instantiate(const Json& scene,const Json& instance,State position);
Json blank(const std::string& id);
}
}
