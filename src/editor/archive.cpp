#include "document.hpp"
#include <cmath>
#include <stdexcept>
#include <map>

namespace physics_editor {
namespace { void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); } }
Json Document::archive() const {
    Json states=Json::array(); std::map<std::string,size_t> pool;
    auto intern=[&](const Json& state) { auto key=state.dump(); auto it=pool.find(key); if(it!=pool.end()) return it->second;
        const auto index=states.size(); pool.emplace(std::move(key),index); states.push_back(state); return index; };
    Json a={{"version",1},{"revision",revision_},{"nextEvent",nextEvent_},{"playback",snapshot()["playback"]},
        {"preview",previewRevision_?Json(*previewRevision_):Json()},{"head",intern(scene_)},{"audit",audit_}};
    for(const auto& name:{"undo","redo","checkpoints"}) a[name]=Json::array();
    for(const auto& e:undo_) a["undo"].push_back({{"state",intern(e.scene)},{"event",e.event}});
    for(const auto& e:redo_) a["redo"].push_back({{"state",intern(e.scene)},{"event",e.event}});
    for(const auto& e:checkpoints_) a["checkpoints"].push_back({{"state",intern(e.scene)},{"event",e.event},{"revision",e.revision}});
    a["states"]=std::move(states); return a;
}
void Document::restoreArchive(const Json& a) {
    require(revision_==0 && audit_.empty() && journalPath_.empty(),"Archive restore requires a fresh document");
    require(a.at("version")==1,"Unsupported document archive version");
    const auto& states=a.at("states"); require(states.is_array() && !states.empty() && states.size()<=302,"Invalid archive state pool");
    const auto& head=states.at(a.at("head").get<size_t>());
    for(const auto& state:states) {
        validate(state);
        require(state.contains("componentDefinition")==head.contains("componentDefinition"),"Mixed document kinds in archive");
        if(head.contains("componentDefinition")) require(state["componentDefinition"]["id"]==head["componentDefinition"]["id"],"Mixed component identities");
    }
    const auto revision=a.at("revision").get<uint64_t>(), next=a.at("nextEvent").get<uint64_t>();
    require(next>0 && revision<next,"Invalid revision/event counters");
    const auto& events=a.at("audit"); require(events.is_array() && events.size()==next-1,"Incomplete project audit");
    uint64_t id=0; for(const auto& e:events) {
        require(e.at("id")==++id && e.at("request").is_object() && e.at("source").is_string() && e.at("utc").is_string(),"Invalid audit entry");
        require(e.at("afterRevision").get<uint64_t>()<=revision && e.at("beforeRevision").get<uint64_t>()<=revision,"Invalid audit revision");
        require(e.at("status")=="success" || e.at("status")=="rejected","Invalid audit status");
    }
    std::vector<UndoEntry> undo,redo;
    auto stack=[&](const char* name,std::vector<UndoEntry>& result) {
        require(a.at(name).is_array() && a.at(name).size()<=100,"Archive undo stack exceeds limit");
        for(const auto& e:a.at(name)) { auto event=e.at("event").get<uint64_t>(); require(event>0 && event<next,"Invalid undo event"); result.push_back({states.at(e.at("state").get<size_t>()),event}); }
    }; stack("undo",undo); stack("redo",redo);
    std::deque<Checkpoint> checkpoints;
    require(a.at("checkpoints").is_array() && !a.at("checkpoints").empty() && a.at("checkpoints").size()<=101,"Invalid checkpoints");
    for(const auto& e:a.at("checkpoints")) {
        auto r=e.at("revision").get<uint64_t>(), event=e.at("event").get<uint64_t>();
        require(r<=revision && event<next && (checkpoints.empty() || r>checkpoints.back().revision),"Invalid checkpoint order");
        checkpoints.push_back({r,event,states.at(e.at("state").get<size_t>())});
    }
    require(checkpoints.back().revision==revision && checkpoints.back().scene==head,"Archive head/checkpoint mismatch");
    std::optional<uint64_t> preview;
    if(!a.at("preview").is_null()) { preview=a["preview"].get<uint64_t>(); bool found=false; for(const auto& c:checkpoints) found|=c.revision==*preview; require(found,"Missing preview checkpoint"); }
    const Json* shown=&head; if(preview) for(const auto& c:checkpoints) if(c.revision==*preview) shown=&c.scene;
    const auto& playback=a.at("playback"); double time=playback.at("time"),rate=playback.at("rate"); int step=playback.at("step");
    require(std::isfinite(time) && time>=0 && time<=shown->at("duration").get<double>() && std::isfinite(rate) && rate>0 && rate<=10 && step>=0 && step<shown->at("steps").get<int>(),"Invalid archived playback");
    require(playback.at("playing").is_boolean(),"Invalid playback flag");
    scene_=head; undo_=std::move(undo); redo_=std::move(redo); checkpoints_=std::move(checkpoints); previewRevision_=preview;
    revision_=revision; nextEvent_=next; time_=time; rate_=rate; step_=step; playing_=false; // Opening a project never starts motion unexpectedly.
    audit_=events.get<std::vector<Json>>(); history_.clear(); historyBytes_=0;
    for(auto it=audit_.rbegin();it!=audit_.rend() && history_.size()<1000;++it) {
        const auto bytes=it->dump().size(); if(historyBytes_+bytes>16*1024*1024 && !history_.empty()) break;
        historyBytes_+=bytes; history_.push_front(*it);
    }
}
}
