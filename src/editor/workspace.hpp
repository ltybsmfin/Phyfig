#pragma once
#include "document.hpp"
#include <memory>

namespace physics_editor {
// Scope is part of the command contract, never inferred from GUI focus.
class Workspace {
public:
    Workspace();
    Document& document(const std::string& scope="scene");
    Json dispatch(const Json& request,const std::string& source="human");
    void startJournal(const std::string& directory);
    void addValidator(std::function<void(const Json&)> validator);
    bool hasComponent() const { return bool(component_); }
    bool dirty(const std::string& scope) const;
    bool dirty() const { return dirty("scene") || (component_ && dirty("component")); }
private:
    Document scene_;
    std::unique_ptr<Document> component_;
    Json savedScene_,savedComponent_;
    std::vector<std::function<void(const Json&)>> validators_;
    std::string journalDirectory_;
    unsigned draftNumber_=0;
};
}
