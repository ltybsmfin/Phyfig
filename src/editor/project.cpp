#include "workspace.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <map>
#include <windows.h>

namespace physics_editor {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void compatibleResources(const Json& saved,const Json& installed) {
    if(!installed.contains("formulaMasks")){require(saved==installed,"Project resources require a compatible editor build");return;}
    require(saved.contains("formulaMasks") && saved["formulaMasks"].is_array(),"Missing project formula masks");
    auto savedMetadata=saved,installedMetadata=installed;savedMetadata.erase("formulaMasks");installedMetadata.erase("formulaMasks");
    require(savedMetadata==installedMetadata,"Project formula metadata is incompatible");
    std::map<std::string,const Json*> masks;
    for(const auto& mask:installed.at("formulaMasks"))masks.emplace(mask.at("name").get<std::string>(),&mask);
    std::map<std::string,bool> seen;
    for(const auto& mask:saved["formulaMasks"]) {
        const auto name=mask.at("name").get<std::string>();require(seen.emplace(name,true).second,"Duplicate project formula resource");
        const auto found=masks.find(name);
        require(found!=masks.end() && *found->second==mask,"Project formula resource is missing or changed in this editor build");
    }
}
}
Json Workspace::projectToken() const {
    return {{"sceneEvent",scene_->history(0,1)["lastId"]},{"componentEvent",component_?component_->history(0,1)["lastId"]:Json()},
        {"componentId",component_?component_->scene()["componentDefinition"]["id"]:Json()},{"closed",closedDrafts_.size()},
        {"layout",layout_},{"events",projectEvents_.size()}};
}
bool Workspace::projectDirty() const { return savedToken_.is_null()?(dirty() || scene_->history(0,1)["lastId"]!=0 || component_ || !closedDrafts_.empty()):savedToken_!=projectToken(); }
Json Workspace::projectInfo() const {
    return {{"path",projectPath_},{"dirty",projectDirty()},{"sceneRevision",scene_->revision()},
        {"componentRevision",component_?Json(component_->revision()):Json()},{"closedDraftCount",closedDrafts_.size()},
        {"token",projectToken()},{"layout",layout_},{"format","physics-project"},{"version",1}};
}
Json Workspace::projectRequest(const Json& request,const std::string& source) {
    const std::string method=request.at("method"); if(method=="get_project")return projectInfo();
    const auto& args=request.at("args");
    require(args.at("path").is_string() && args["path"].get_ref<const std::string&>().size()<2000,"Project path too long");
    if(args.contains("expectedToken"))require(args["expectedToken"]==projectToken(),"Project conflict: re-read get_project before saving/opening");
    else require(source!="mcp","MCP project mutations require expectedToken");
    const auto path=std::filesystem::absolute(std::filesystem::u8path(args.at("path").get<std::string>()));
    require(path.extension()==L".physproj","Project path must end in .physproj");
    if(method=="save_project") {
        require(args.value("overwrite",false) || !std::filesystem::exists(path),"Project exists; explicit overwrite required");
        auto events=projectEvents_; events.push_back({{"method",method},{"source",source},{"sequence",events.size()+1}});
        Json package={{"format","physics-project"},{"version",1},{"scene",scene_->archive()},
            {"component",component_?component_->archive():Json()},{"closedDrafts",closedDrafts_},
            {"layout",layout_},{"resources",resources_},{"projectEvents",events}};
        const auto bytes=package.dump(); require(bytes.size()<=256*1024*1024,"Project exceeds 256 MB; history was not discarded");
        if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
        auto temporary=path; temporary+=L".writing"; require(!std::filesystem::exists(temporary),"Project temporary file exists");
        try {
            {std::ofstream stream(temporary,std::ios::binary);require(bool(stream),"Cannot create project");stream<<bytes;stream.flush();require(bool(stream),"Cannot write project");}
            require(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot atomically replace project");
        }catch(...){std::filesystem::remove(temporary);throw;}
        projectEvents_=std::move(events); projectPath_=path.u8string();
        savedScene_=scene_->snapshot()["document"];savedComponent_=component_?component_->snapshot()["document"]:Json();savedToken_=projectToken();
    } else {
        require(!projectDirty() || args.value("discard",false),"Unsaved project; save first or explicitly discard");
        require(std::filesystem::file_size(path)<=256*1024*1024,"Project exceeds 256 MB");
        std::ifstream stream(path,std::ios::binary);require(bool(stream),"Cannot read project");Json package;stream>>package;
        require(package.at("format")=="physics-project" && package.at("version")==1,"Unsupported project format");
        auto stage=[&](const Json& archive) {
            auto doc=std::make_unique<Document>();for(const auto& validator:validators_)doc->addValidator(validator);doc->restoreArchive(archive);return doc;
        };
        auto nextScene=stage(package.at("scene"));require(!nextScene->scene().contains("componentDefinition"),"Main scene is a component");
        std::unique_ptr<Document> nextComponent;
        if(!package.at("component").is_null()){nextComponent=stage(package["component"]);require(nextComponent->scene().contains("componentDefinition"),"Draft is not a component");}
        const auto& closed=package.at("closedDrafts");require(closed.is_array(),"Invalid closed draft collection");
        for(const auto& archive:closed){auto doc=stage(archive);require(doc->scene().contains("componentDefinition"),"Invalid closed draft");}
        require(package.at("layout").is_object() && package["layout"].dump().size()<=16384,"Invalid layout data");
        const auto& layout=package["layout"];
        for(const char* key:{"workspace","scope","selected","channel"})if(layout.contains(key))require(layout[key].is_string() && layout[key].get_ref<const std::string&>().size()<1024,"Invalid layout string");
        for(const char* key:{"sidebarWidth","bottomHeight","zoom"})if(layout.contains(key))require(layout[key].is_number() && layout[key]>=0 && layout[key]<=10000,"Invalid layout dimensions");
        if(layout.contains("referenceZoom"))require(layout["referenceZoom"].is_number() && layout["referenceZoom"]>=8 && layout["referenceZoom"]<=1200,"Invalid pixel reference zoom");
        for(const char* key:{"historyOpen","grid","trails","followMcp"})if(layout.contains(key))require(layout[key].is_boolean(),"Invalid layout flag");
        if(layout.contains("pan")) {require(layout["pan"].is_array() && layout["pan"].size()==2,"Invalid pan");for(const auto& v:layout["pan"])require(v.is_number() && v>=-1e6 && v<=1e6,"Invalid pan offset");}
        require(package.at("resources").is_object(),"Invalid project resources");
        // Additions to the installed library are backward compatible; changing an existing mask is not.
        if(!resources_.empty())compatibleResources(package["resources"],resources_);
        auto events=package.at("projectEvents");require(events.is_array(),"Invalid project event log");
        events.push_back({{"method",method},{"source",source},{"sequence",events.size()+1}});
        if(!journalDirectory_.empty()) {
            const auto base=std::filesystem::u8path(journalDirectory_)/("project-"+std::to_string(++draftNumber_));
            nextScene->startJournal((base/"scene").u8string());if(nextComponent)nextComponent->startJournal((base/"component").u8string());
        }
        // Every document and all history states have been validated before this swap.
        scene_=std::move(nextScene);component_=std::move(nextComponent);closedDrafts_=closed;layout_=package["layout"];
        // Keep the full installed catalog so a subsequent save also packages newly available assets.
        if(resources_.empty())resources_=package["resources"];
        projectEvents_=std::move(events);projectPath_=path.u8string();savedScene_=scene_->snapshot()["document"];savedComponent_=component_?component_->snapshot()["document"]:Json();savedToken_=projectToken();
    }
    auto result=projectInfo();result["scope"]="scene";return result;
}
}
