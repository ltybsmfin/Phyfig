#include "workspace.hpp"
#include "formula_resources.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <map>
#include <cstdint>
#include <cstdio>
#include <windows.h>

namespace physics_editor {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
// Layout is presentation state, not document state: it is saved and loaded, but
// excluded from the concurrency token so panning/zooming/resizing never reports
// a project as dirty or blocks a concurrent save. Documents, resources, history
// and drafts remain covered by the same token.
void mixBytes(uint64_t& hash,const void* data,size_t bytes) {
    const auto* raw=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<bytes;++i) { hash^=raw[i]; hash*=1099511628211ull; }
    const unsigned char separator=0xFF; hash^=separator; hash*=1099511628211ull;
}
void mixString(uint64_t& hash,const std::string& text) { mixBytes(hash,text.data(),text.size()); }
void mixLength(uint64_t& hash,uint64_t length) { mixBytes(hash,&length,sizeof(length)); }
// Mixes every JSON value with an explicit type tag, and gives strings and
// containers an explicit length, so an empty array, an empty object, an empty
// string and null can never collapse onto the same bytes. Unknown value types
// fall back to dump().
void mixValue(uint64_t& hash,const Json& value) {
    switch(value.type()) {
    case Json::value_t::null:            { const unsigned char tag=1; mixBytes(hash,&tag,1); return; }
    case Json::value_t::boolean:         { const unsigned char tag=2,bit=value.get<bool>()?1:0; mixBytes(hash,&tag,1); mixBytes(hash,&bit,1); return; }
    case Json::value_t::number_integer:  { const unsigned char tag=3; const int64_t number=value.get<int64_t>(); mixBytes(hash,&tag,1); mixBytes(hash,&number,sizeof(number)); return; }
    case Json::value_t::number_unsigned: { const unsigned char tag=4; const uint64_t number=value.get<uint64_t>(); mixBytes(hash,&tag,1); mixBytes(hash,&number,sizeof(number)); return; }
    case Json::value_t::number_float:    { const unsigned char tag=5; const double number=value.get<double>(); mixBytes(hash,&tag,1); mixBytes(hash,&number,sizeof(number)); return; }
    case Json::value_t::string:          { const unsigned char tag=6; const auto& text=value.get_ref<const std::string&>(); mixBytes(hash,&tag,1); mixLength(hash,text.size()); mixString(hash,text); return; }
    case Json::value_t::array:           { const unsigned char tag=7; mixBytes(hash,&tag,1); mixLength(hash,value.size()); for(const auto& element:value) mixValue(hash,element); return; }
    case Json::value_t::object:          { const unsigned char tag=8; mixBytes(hash,&tag,1); mixLength(hash,value.size()); for(auto it=value.begin();it!=value.end();++it) { mixLength(hash,it.key().size()); mixString(hash,it.key()); mixValue(hash,it.value()); } return; }
    default:                             { const unsigned char tag=9; mixBytes(hash,&tag,1); mixString(hash,value.dump()); return; }
    }
}
// Complete resource fingerprint: name, dimensions, latex, the full RLE payload
// and any extra field all participate. Iterating the DOM instead of dump()ing
// the masks avoids building a multi-megabyte string, and Workspace caches the
// result until setResources() or a project load invalidates it.
std::string resourceFingerprint(const Json& resources) {
    uint64_t hash=14695981039346656037ull; // FNV-1a 64-bit offset basis
    mixValue(hash,resources);
    char text[32]; std::snprintf(text,sizeof(text),"%016llx",static_cast<unsigned long long>(hash));
    return text;
}
// Two same-named formula assets must agree on everything the bitmap depends on:
// name, dimensions, the full RLE payload, the LaTeX source and any extension
// field. A changed mask is a real collision and is rejected.
//
// The structural writing plan is additive metadata rather than bitmap content: a
// project saved before plans existed must still load against a library that has
// one, and a project that carries a plan must still load against a library that
// does not. The installed asset wins, and additive fields the installed asset is
// missing are carried over from the project (see compatibleResources).
void requireCompatibleAsset(const Json& saved,const Json& installed) {
    require(saved.is_object() && installed.is_object(),"Invalid project formula resource");
    Json project=saved,library=installed;
    project.erase("writingPlan");library.erase("writingPlan");
    if(!project.contains("latex") && library.contains("latex"))library.erase("latex");
    if(!library.contains("latex") && project.contains("latex"))project.erase("latex");
    require(project==library,"Project formula id conflicts with an existing asset");
}
Json compatibleResources(const Json& saved,const Json& installed) {
    formula_resources::validate(saved);
    if(installed.empty())return saved;
    if(!installed.contains("formulaMasks")){require(saved==installed,"Project resources require a compatible editor build");return saved;}
    require(saved.contains("formulaMasks") && saved["formulaMasks"].is_array(),"Missing project formula masks");
    auto savedMetadata=saved,installedMetadata=installed;savedMetadata.erase("formulaMasks");installedMetadata.erase("formulaMasks");
    require(savedMetadata==installedMetadata,"Project formula metadata is incompatible");
    std::map<std::string,const Json*> masks;
    for(const auto& mask:installed.at("formulaMasks"))masks.emplace(mask.at("name").get<std::string>(),&mask);
    auto merged=installed;
    std::map<std::string,std::size_t> positions;
    for(std::size_t i=0;i<merged.at("formulaMasks").size();++i)positions[merged["formulaMasks"][i].at("name").get<std::string>()]=i;
    std::map<std::string,bool> seen;
    for(const auto& mask:saved["formulaMasks"]) {
        const auto name=mask.at("name").get<std::string>();require(seen.emplace(name,true).second,"Duplicate project formula resource");
        const auto found=masks.find(name);
        if(found==masks.end())merged["formulaMasks"].push_back(mask);
        else {
            requireCompatibleAsset(mask,*found->second);
            // Keep additive metadata that only the project has (a plan or source
            // text written by a newer build than the installed library).
            Json& target=merged["formulaMasks"][positions.at(name)];
            for(const char* key:{"latex","writingPlan"})if(!target.contains(key) && mask.contains(key))target[key]=mask[key];
        }
    }
    formula_resources::validate(merged);return merged;
}
}
Json Workspace::projectToken() const {
    // setResources() and project load drop the cache; otherwise the fingerprint
    // is reused, so the per-frame dirty poll stays O(1).
    if(!resourceFingerprintValid_) {
        resourceFingerprint_=resourceFingerprint(*resources_);
        resourceFingerprintValid_=true;
    }
    return {{"sceneEvent",scene_->history(0,1)["lastId"]},{"componentEvent",component_?component_->history(0,1)["lastId"]:Json()},
        {"componentId",component_?component_->scene()["componentDefinition"]["id"]:Json()},{"closed",closedDrafts_.size()},
        {"resources",resourceFingerprint_},
        {"events",projectEvents_.size()}};
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
            {"layout",layout_},{"resources",*resources_},{"projectEvents",events}};
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
        // Only built-ins are inherited across projects; custom assets belong to
        // the opened package, not every project previously visited this session.
        auto nextResources=std::make_shared<Json>(compatibleResources(package.at("resources"),checkFormulas_?installedResources_:*resources_));
        auto stage=[&](const Json& archive) {
            auto initial=Document::demo();initial["objects"]=Json::array();
            auto doc=std::make_unique<Document>(Registry{},initial);for(const auto& validator:validators_)doc->addValidator(validator);if(checkFormulas_)doc->addValidator([r=nextResources](const Json& s){formula_resources::scene(s,*r);});doc->restoreArchive(archive);return doc;
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
        formula_resources::validate(*nextResources);
        auto events=package.at("projectEvents");require(events.is_array(),"Invalid project event log");
        events.push_back({{"method",method},{"source",source},{"sequence",events.size()+1}});
        if(!journalDirectory_.empty()) {
            const auto base=std::filesystem::u8path(journalDirectory_)/("project-"+std::to_string(++draftNumber_));
            nextScene->startJournal((base/"scene").u8string());if(nextComponent)nextComponent->startJournal((base/"component").u8string());
        }
        // Every document and all history states have been validated before this swap.
        scene_=std::move(nextScene);component_=std::move(nextComponent);closedDrafts_=closed;layout_=package["layout"];
        // Keep the full installed catalog so a subsequent save also packages newly available assets.
        resources_=std::move(nextResources);resourceFingerprintValid_=false;
        projectEvents_=std::move(events);projectPath_=path.u8string();savedScene_=scene_->snapshot()["document"];savedComponent_=component_?component_->snapshot()["document"]:Json();savedToken_=projectToken();
    }
    auto result=projectInfo();result["scope"]="scene";return result;
}
}
