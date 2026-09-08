#pragma once
#include "json.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <optional>

namespace physics_editor {
using Json = nlohmann::json;

// Registrations own defaults, validation and public descriptions. Persistent type
// names, rather than enum ordinals, form the document and automation contract.
struct Primitive {
    std::string label;
    Json defaults;
    std::function<void(const Json&)> validate;
};
class Registry {
public:
    Registry();
    void add(std::string type, Primitive definition);
    const Primitive& get(const std::string& type) const;
    Json describe() const;
private:
    std::map<std::string, Primitive> types_;
};

struct State { double x, y, vx, vy; };
class Document {
public:
    using Command = std::function<void(Json&, const Json&)>;
    explicit Document(Registry registry = {},Json initial=Json());
    Document(const Document&)=delete;
    Document& operator=(const Document&)=delete;
    void registerCommand(std::string name, std::string description, Command command);
    void addValidator(std::function<void(const Json&)> validator);
    Json dispatch(const Json& request, const std::string& source="system");
    void startJournal(const std::string& directory);
    Json history(uint64_t after=0, size_t limit=100) const;
    const std::string& journalError() const { return journalError_; }
    Json snapshot() const;
    Json catalog() const;
    const Json& scene() const;
    bool previewing() const { return previewRevision_.has_value(); }
    uint64_t displayRevision() const { return previewRevision_.value_or(revision_); }
    Json timeline() const;
    const Registry& registry() const { return registry_; }
    const Json& object(const std::string& id) const;
    State evaluate(const std::string& id, double time) const;
    void advance(double dt);
    double time() const { return time_; }
    bool playing() const { return playing_; }
    int step() const { return step_; }
    uint64_t revision() const { return revision_; }
    static Json demo();
    Json archive() const;
    void restoreArchive(const Json& archive); // Restore into a fresh, staged document only.
private:
    Registry registry_;
    Json scene_;
    std::map<std::string, std::pair<std::string, Command>> commands_;
    struct UndoEntry { Json scene; uint64_t event; };
    std::vector<UndoEntry> undo_, redo_;
    std::deque<Json> history_;
    std::vector<Json> audit_;
    size_t historyBytes_=0;
    uint64_t nextEvent_=1, relatedEvent_=0;
    struct Checkpoint { uint64_t revision,event; Json scene; };
    std::deque<Checkpoint> checkpoints_;
    std::optional<uint64_t> previewRevision_;
    void checkpoint(uint64_t event);
    std::string journalPath_, journalError_;
    std::vector<std::function<void(const Json&)>> validators_;
    uint64_t revision_ = 0;
    double time_ = 0, rate_ = 0.6;
    bool playing_ = false;
    int step_ = 3;
    void validate(const Json& scene) const;
    void commit(Json next);
    Json executeRequest(const Json& request);
    void record(Json event);
};
}
