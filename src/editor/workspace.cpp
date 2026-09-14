#include "workspace.hpp"
#include "component.hpp"
#include "formula_resources.hpp"
#include <filesystem>
#include <set>
#include <stdexcept>
#include <utility>

namespace physics_editor {
namespace { void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); } }
Workspace::Workspace():scene_(std::make_unique<Document>()),savedScene_(scene_->scene()) {}
// Whole-resource commit: the shared JSON object keeps its identity, so formula
// validators captured as shared_ptr keep seeing the current library, while the
// cached fingerprint is dropped so the next token reflects the new content.
void Workspace::setResources(Json resources) {
    *resources_=std::move(resources);
    resourceFingerprintValid_=false;
}
void Workspace::validateFormulas(){
    if(checkFormulas_)return;
    formula_resources::validate(*resources_);
    scene_->addValidator([r=resources_](const Json& s){formula_resources::scene(s,*r);});
    if(component_)component_->addValidator([r=resources_](const Json& s){formula_resources::scene(s,*r);});
    installedResources_=*resources_;checkFormulas_=true;
}
Document& Workspace::document(const std::string& scope) {
    require(scope=="scene" || scope=="component","scope must be scene or component");
    if(scope=="scene") return *scene_;
    require(bool(component_),"No component draft open"); return *component_;
}
bool Workspace::dirty(const std::string& scope) const {
    return scope=="scene"?scene_->snapshot()["document"]!=savedScene_:
        component_ && component_->snapshot()["document"]!=savedComponent_;
}
void Workspace::startJournal(const std::string& directory) {
    scene_->startJournal(directory); journalDirectory_=directory;
}
void Workspace::addValidator(std::function<void(const Json&)> validator) {
    scene_->addValidator(validator); if(component_) component_->addValidator(validator); validators_.push_back(std::move(validator));
}
Json Workspace::dispatch(const Json& request,const std::string& source) {
    const std::string method=request.at("method"),scope=request.value("scope",std::string("scene"));
    require(scope=="scene" || scope=="component","scope must be scene or component");
    const auto args=request.value("args",Json::object());
    if(method=="get_project" || method=="save_project" || method=="load_project") return projectRequest(request,source);
    if(method=="open_component") {
        require(!scene_->previewing(),"Exit scene history preview before opening a draft");
        if(request.contains("expectedRevision")) require(request["expectedRevision"]==scene_->revision(),"Scene revision conflict");
        require(!component_,"Close or save the existing draft before opening another");
        const std::string id=args.at("id"); Json definition;
        if(scene_->scene().contains("components") && scene_->scene()["components"].contains(id)) definition=scene_->scene()["components"][id];
        else {
            definition=components::blank(id);
            const auto ids=args.value("objects",Json::array()); require(ids.is_array(),"objects must be an array of scene object ids");
            std::set<std::string> unique;
            for(const auto& item:ids) {
                const std::string objectId=item; require(unique.insert(objectId).second,"Duplicate child selection");
                definition["objects"].push_back(scene_->object(objectId));
            }
            definition["steps"]=scene_->scene()["steps"];
        }
        auto draft=std::make_unique<Document>(Registry{},definition);
        for(const auto& validator:validators_) draft->addValidator(validator);
        if(checkFormulas_)draft->addValidator([r=resources_](const Json& s){formula_resources::scene(s,*r);});
        if(!journalDirectory_.empty()) draft->startJournal((std::filesystem::u8path(journalDirectory_)/"components"/std::to_string(++draftNumber_)).u8string());
        savedComponent_=definition; component_=std::move(draft);
        auto result=component_->snapshot(); result["scope"]="component"; return result;
    }
    if(method=="close_component") {
        require(bool(component_),"No component draft open");
        require(!dirty("component") || args.value("discard",false),"Unsaved component draft; save it or explicitly discard");
        closedDrafts_.push_back(component_->archive()); component_.reset(); return {{"closed",true},{"scope","scene"}};
    }
    if(method=="publish_component") {
        auto& draft=document("component");
        require(!draft.previewing(),"Exit component history preview before publishing");
        require(args.at("componentRevision")==draft.revision(),"Component revision conflict");
        require(request.at("expectedRevision")==scene_->revision(),"Scene revision conflict");
        auto result=scene_->dispatch({{"method","apply"},{"expectedRevision",request["expectedRevision"]},
            {"args",{{"commands",Json::array({{{"op","install_component"},{"definition",draft.scene()}}})}}}},source);
        result["scope"]="scene"; return result;
    }
    auto& doc=document(scope);
    auto result=doc.dispatch(request,source); result["scope"]=scope;
    if(method=="load" || method=="save") (scope=="scene"?savedScene_:savedComponent_)=doc.snapshot()["document"];
    return result;
}
}
