#pragma once
#include "document.hpp"
#include <cstdint>
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
    void validateFormulas();
    bool hasComponent() const { return bool(component_); }
    bool dirty(const std::string& scope) const;
    bool dirty() const { return dirty("scene") || (component_ && dirty("component")); }
    Json& layout() { return layout_; }
    // Single read accessor plus a whole-resource commit. An escaping reference
    // can no longer mutate the library behind the project token's back; every
    // writer goes through setResources(), which invalidates the fingerprint.
    const Json& resources() const { return *resources_; }
    void setResources(Json resources);
    // Bumped on every whole-resource commit. Derived caches (the Write entry mask
    // geometry) key on it instead of hashing the mask payload every frame.
    uint64_t resourceGeneration() const { return resourceGeneration_; }
    Json projectInfo() const;
    bool projectDirty() const;
private:
    std::unique_ptr<Document> scene_;
    std::unique_ptr<Document> component_;
    Json savedScene_,savedComponent_;
    std::vector<std::function<void(const Json&)>> validators_;
    std::string journalDirectory_;
    unsigned draftNumber_=0;
    Json closedDrafts_=Json::array(),layout_=Json::object();
    std::shared_ptr<Json> resources_=std::make_shared<Json>(Json::object());
    uint64_t resourceGeneration_=0;
    Json savedToken_, projectEvents_=Json::array();
    std::string projectPath_;
    mutable std::string resourceFingerprint_;
    mutable bool resourceFingerprintValid_=false;
    bool checkFormulas_=false;
    Json installedResources_;
    Json projectRequest(const Json& request,const std::string& source);
    Json projectToken() const;
};
}
