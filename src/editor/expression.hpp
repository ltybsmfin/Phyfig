#pragma once
#include "json.hpp"
#include <memory>
#include <set>
#include <string>

namespace physics_editor {
// Compiled, bounded arithmetic tree: no execution, file access, or external state.
class Expression {
public:
    struct Node;
    explicit Expression(const std::string& source,const std::set<std::string>& variables);
    double evaluate(const nlohmann::json& variables) const;
private:
    std::shared_ptr<const Node> root_;
};
}
