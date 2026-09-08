#include "document.hpp"
#include "component.hpp"
#include "animation.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <cstdio>

namespace physics_editor {
namespace {
std::string utcNow() {
    SYSTEMTIME time{}; GetSystemTime(&time); char text[40];
    std::snprintf(text,sizeof(text),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);
    return text;
}
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
double number(const Json& j, const char* key) {
    require(j.contains(key) && j[key].is_number(), std::string(key) + " must be a number");
    const double n = j[key].get<double>();
    require(std::isfinite(n) && std::abs(n) <= 1e6, std::string(key) + " must be finite and within +/- 1000000");
    return n;
}
Json& find(Json& scene, const std::string& id) {
    for (auto& o : scene.at("objects")) if (o.at("id") == id) return o;
    throw std::runtime_error("Unknown object: " + id);
}
void boundedText(const Json& j, const char* key, size_t max) {
    require(j.contains(key) && j[key].is_string() && j[key].get_ref<const std::string&>().size() <= max,
        std::string(key) + " must be a bounded UTF-8 string");
}
}

Registry::Registry() {
    auto shape = [](const Json& p) {
        number(p,"x"); number(p,"y");
        const double width = number(p,"stroke"); require(width > 0 && width <= 20,"stroke must be in (0,20]");
        require(p.at("color").is_array() && p.at("color").size() == 4,"color requires four RGBA components");
        for (auto& c : p.at("color")) require(c.is_number() && c.get<double>() >= 0 && c.get<double>() <= 1,"color component outside [0,1]");
    };
    const Json base = {{"x",0.0},{"y",0.0},{"stroke",2.0},{"color",{0.12,0.22,0.32,1.0}}};
    auto addShape = [&](const std::string& name,const std::string& label,Json extra,auto validate) {
        Json defaults = base; defaults.update(extra);
        add(name,{label,defaults,[shape,validate](const Json& p) { shape(p); validate(p); }});
    };
    auto radius = [](const Json& p) { require(number(p,"radius") > 0,"radius must be positive"); };
    auto endpoint = [](const Json& p) { number(p,"dx"); number(p,"dy"); };
    addShape("point","质点",{{"radius",0.16}},radius);
    addShape("circle","圆",{{"radius",1.0}},radius);
    addShape("arc","圆弧",{{"radius",1.0},{"start",180.0},{"end",360.0}},[radius](const Json& p) {
        radius(p); const double sweep=number(p,"end")-number(p,"start"); require(sweep>0 && sweep<=360,"Arc must sweep counterclockwise in (0,360] degrees");
    });
    addShape("component","组件实例",{{"definition","component"},{"parameters",Json::object()},{"rotation",0.0},{"scale",1.0}},[](const Json& p) {
        boundedText(p,"definition",128); require(p.at("parameters").is_object(),"Instance parameters must be an object");
        number(p,"rotation"); require(number(p,"scale")>0 && p["scale"].get<double>()<=100,"Instance scale must be in (0,100]");
    });
    addShape("line","线段",{{"dx",3.0},{"dy",0.0}},endpoint);
    addShape("arrow","矢量箭头",{{"dx",0.0},{"dy",3.0}},endpoint);
    addShape("ground","地面",{{"dx",12.0},{"dy",0.0}},endpoint);
    addShape("rectangle","矩形",{{"width",3.0},{"height",2.0},{"rotation",0.0}},[](const Json& p) {
        require(number(p,"width") > 0 && number(p,"height") > 0,"rectangle dimensions must be positive");
        if(p.contains("rotation")) number(p,"rotation");
    });
    addShape("text","文字",{{"text","文字"},{"size",20.0}},[](const Json& p) {
        boundedText(p,"text",4096); require(number(p,"size") >= 8 && number(p,"size") <= 96,"text size must be in [8,96]");
    });
    addShape("formula","LaTeX 公式",{{"formula","position_y"},{"scale",1.0}},[](const Json& p) {
        boundedText(p,"formula",100); require(number(p,"scale") >= 0.25 && number(p,"scale") <= 4,"formula scale must be in [0.25,4]");
    });
    addShape("polyline","折线",{{"points",{{0.0,0.0},{1.0,2.0},{3.0,1.0}}}},[](const Json& p) {
        const auto& points=p.at("points"); require(points.is_array() && points.size() >= 2 && points.size() <= 1024,"polyline requires 2..1024 points");
        for (auto& v:points) { require(v.is_array() && v.size()==2,"point requires x,y"); for(auto& n:v) require(n.is_number() && std::isfinite(n.get<double>()) && std::abs(n.get<double>()) <= 1e6,"invalid point coordinate"); }
    });
}
void Registry::add(std::string name,Primitive primitive) {
    require(!name.empty() && !types_.count(name),"Duplicate/empty primitive type");
    primitive.validate(primitive.defaults); types_.emplace(std::move(name),std::move(primitive));
}
const Primitive& Registry::get(const std::string& name) const {
    const auto it=types_.find(name); require(it!=types_.end(),"Unsupported primitive type: "+name); return it->second;
}
Json Registry::describe() const {
    Json result=Json::array();
    for(const auto& [type,p]:types_) result.push_back({{"type",type},{"label",p.label},{"defaults",p.defaults}});
    return result;
}

Document::Document(Registry registry,Json initial):registry_(std::move(registry)),scene_(initial.is_null()?demo():std::move(initial)) {
    registerCommand("create","Create a primitive; id must be unique",[this](Json& s,const Json& a) {
        const auto type=a.at("type").get<std::string>(); Json properties=registry_.get(type).defaults;
        if(a.contains("properties")) properties.update(a.at("properties"));
        Json o={{"id",a.at("id")},{"type",type},{"name",a.value("name",a.at("id").get<std::string>())},
            {"visible",true},{"locked",false},{"step",0},{"properties",properties}};
        for(const char* field:{"motion","binding","animation","step","visible","locked"}) if(a.contains(field)) o[field]=a[field];
        s["objects"].push_back(std::move(o));
    });
    registerCommand("update","Merge object fields and properties; null removes optional motion/binding",[](Json& s,const Json& a) {
        auto& o=find(s,a.at("id")); const auto& patch=a.at("patch"); require(patch.is_object(),"patch must be an object");
        require(!patch.contains("id") && !patch.contains("type"),"id/type are immutable; delete and create instead");
        o.merge_patch(patch);
    });
    registerCommand("delete","Delete an object; referenced targets cannot be deleted unless dependants are also changed in this batch",[](Json& s,const Json& a) {
        const auto id=a.at("id").get<std::string>(); find(s,id);
        auto& objects=s["objects"]; objects.erase(std::remove_if(objects.begin(),objects.end(),[&](const Json& o){return o["id"]==id;}),objects.end());
    });
    registerCommand("move","Translate initial position in world coordinates (meters)",[](Json& s,const Json& a) {
        auto& p=find(s,a.at("id"))["properties"]; p["x"]=p.at("x").get<double>()+number(a,"dx"); p["y"]=p.at("y").get<double>()+number(a,"dy");
    });
    registerCommand("reorder","Set object drawing index; later objects are in front",[](Json& s,const Json& a) {
        Json o=find(s,a.at("id")); const int index=a.at("index"); auto& objects=s["objects"];
        require(index>=0 && index<static_cast<int>(objects.size()),"index outside object list");
        objects.erase(std::remove_if(objects.begin(),objects.end(),[&](const Json& v){return v["id"]==o["id"];}),objects.end());
        objects.insert(objects.begin()+index,std::move(o));
    });
    registerCommand("settings","Update scene title, duration and number of reveal steps",[](Json& s,const Json& a) {
        for(const char* key:{"title","duration","steps"}) if(a.contains(key)) s[key]=a[key];
    });
    registerCommand("install_component","Publish a definition snapshot into this scene as one transaction",[](Json& s,const Json& a) {
        require(!s.contains("componentDefinition"),"Publish components into a scene, not a definition");
        const auto& definition=a.at("definition"); const std::string id=definition.at("componentDefinition").at("id");
        if(!s.contains("components")) s["components"]=Json::object(); s["components"][id]=definition;
    });
    registerCommand("component_settings","Replace public parameter/binding metadata in a definition document",[](Json& s,const Json& a) {
        require(s.contains("componentDefinition"),"Open a component definition first");
        s["componentDefinition"].merge_patch(a.at("patch"));
    });
    registerCommand("set_animation","Set authored animation or remove it with null",[](Json& s,const Json& a){
        auto& o=find(s,a.at("id"));if(a.at("animation").is_null())o.erase("animation");else o["animation"]=a["animation"];
    });
    registerCommand("set_keyframe","Insert/update one keyframe in an existing key channel",[](Json& s,const Json& a){
        auto& channel=find(s,a.at("id")).at("animation").at("channels").at(a.at("channel").get<std::string>());
        require(channel.at("kind")=="keys","Select a keyframe channel");auto& keys=channel.at("keys");const double time=number(a,"time");
        Json key={{"time",time},{"value",number(a,"value")},{"interpolation",a.value("interpolation",std::string("linear"))}};
        auto it=std::find_if(keys.begin(),keys.end(),[&](const Json& k){return k.at("time")==time;});if(it==keys.end())keys.push_back(key);else *it=key;
        std::sort(keys.begin(),keys.end(),[](const Json& left,const Json& right){return left["time"].get<double>()<right["time"].get<double>();});
    });
    registerCommand("delete_keyframe","Remove one keyframe; a channel must retain a key",[](Json& s,const Json& a){
        auto& channel=find(s,a.at("id")).at("animation").at("channels").at(a.at("channel").get<std::string>());
        require(channel.at("kind")=="keys","Select a keyframe channel");auto& keys=channel.at("keys");const double time=number(a,"time");
        auto it=std::find_if(keys.begin(),keys.end(),[&](const Json& k){return k.at("time")==time;});require(it!=keys.end(),"Keyframe not found");keys.erase(it);
    });
    validate(scene_);
    step_=std::min(step_,scene_["steps"].get<int>()-1);
    checkpoint(0);
}
void Document::registerCommand(std::string name,std::string description,Command command) {
    require(!name.empty() && !commands_.count(name),"Duplicate/empty command");
    commands_.emplace(std::move(name),std::make_pair(std::move(description),std::move(command)));
}
void Document::addValidator(std::function<void(const Json&)> validator) {
    validator(scene_); validators_.push_back(std::move(validator));
}
void Document::validate(const Json& s) const {
    require(s.is_object() && s.at("version")==1,"Unsupported document version (expected 1)");
    boundedText(s,"title",256);
    require(number(s,"duration")>0 && s.at("duration").get<double>()<=3600,"duration must be in (0,3600]");
    require(s.at("steps").is_number_integer() && s.at("steps")>=1 && s.at("steps")<=100,"steps must be in [1,100]");
    require(s.at("objects").is_array() && s.at("objects").size()<=1000,"scene allows at most 1000 objects");
    std::set<std::string> ids;
    for(const auto& o:s.at("objects")) {
        boundedText(o,"id",128); boundedText(o,"type",128); boundedText(o,"name",256);
        const auto id=o.at("id").get<std::string>(); require(!id.empty() && ids.insert(id).second,"Duplicate/empty object id: "+id);
        require(o.at("visible").is_boolean() && o.at("locked").is_boolean(),"visible/locked must be booleans");
        require(o.at("step").is_number_integer() && o.at("step")>=0 && o.at("step")<s.at("steps"),"object reveal step outside scene steps");
        registry_.get(o.at("type")).validate(o.at("properties"));
        animation::validate(o,s["duration"],registry_);
        if(o.contains("motion")) { const auto& m=o.at("motion"); for(auto key:{"vx","vy","ax","ay"}) number(m,key); }
        if(o.contains("binding")) {
            require(!o.contains("motion"),"A bound object cannot have its own motion");
            const auto& b=o.at("binding"); boundedText(b,"target",128);
            require(b.at("kind")=="position" || (b.at("kind")=="velocity" && o.at("type")=="arrow"),"binding must be position, or velocity on an arrow");
            require(number(b,"scale")>0 && b.at("scale").get<double>()<=100,"binding scale must be in (0,100]");
        }
    }
    for(const auto& o:s.at("objects")) if(o.contains("binding")) {
        const auto target=o["binding"]["target"].get<std::string>(); require(ids.count(target)!=0,"Dangling binding: "+target);
        // A flat binding graph is sufficient for the first editor; disallow cycles
        // and ambiguous chained coordinate systems explicitly instead of guessing.
        const auto it=std::find_if(s["objects"].begin(),s["objects"].end(),[&](const Json& v){return v["id"]==target;});
        require(it->at("id")!=o.at("id") && !it->contains("binding"),"Binding targets must be independent objects");
        if(o["binding"]["kind"]=="velocity" && it->contains("animation")) {
            const auto& channels=it->at("animation").at("channels");
            require((!channels.contains("x") || channels.contains("vx")) && (!channels.contains("y") || channels.contains("vy")),"Authored x/y animation requires explicit vx/vy channels for velocity arrows; no physical derivative is implied");
        }
    }
    require(s.dump().size()<=900000,"Document exceeds 900 KB limit");
    if(s.contains("componentDefinition")) components::validateDefinition(s,registry_);
    if(s.contains("components")) {
        require(s["components"].is_object() && s["components"].size()<=32,"Scene allows at most 32 definitions");
        for(auto it=s["components"].begin();it!=s["components"].end();++it) {
            require(it.value().contains("componentDefinition") && it.value()["componentDefinition"]["id"]==it.key(),"Definition key/id mismatch");
            require(!it.value().contains("components") || it.value()["components"].empty(),"Nested component libraries are not supported");
            validate(it.value());
        }
    }
    for(const auto& o:s["objects"]) if(o["type"]=="component") {
        for(const auto& child:components::instantiate(s,o,{0,0,0,0})) registry_.get(child.at("type")).validate(child.at("properties"));
    }
    for(const auto& validator:validators_) validator(s);
}
void Document::commit(Json next) {
    require(next.contains("componentDefinition")==scene_.contains("componentDefinition"),"Scene and component definitions are separate document kinds");
    if(scene_.contains("componentDefinition")) require(next["componentDefinition"].at("id")==scene_["componentDefinition"]["id"],"Component draft identity is immutable");
    validate(next); if(next==scene_) return;
    undo_.push_back({scene_,nextEvent_}); if(undo_.size()>100) undo_.erase(undo_.begin());
    redo_.clear(); scene_=std::move(next); ++revision_; playing_=false;
    time_=std::min(time_,scene_["duration"].get<double>()); step_=std::min(step_,scene_["steps"].get<int>()-1);
}
Json Document::snapshot() const {
    return {{"revision",revision_},{"document",scene_},{"displayRevision",displayRevision()},{"previewing",previewing()},
        {"playback",{{"time",time_},{"rate",rate_},{"playing",playing_},{"step",step_}}},{"canUndo",!undo_.empty()},{"canRedo",!redo_.empty()}};
}
void Document::checkpoint(uint64_t event) {
    checkpoints_.push_back({revision_,event,scene_});
    if(checkpoints_.size()>101) checkpoints_.pop_front();
}
const Json& Document::scene() const {
    if(previewRevision_) for(const auto& c:checkpoints_) if(c.revision==*previewRevision_) return c.scene;
    return scene_;
}
Json Document::timeline() const {
    Json states=Json::array(); for(const auto& c:checkpoints_) states.push_back({{"revision",c.revision},{"event",c.event}});
    return {{"states",states},{"headRevision",revision_},{"displayRevision",displayRevision()},{"previewing",previewing()}};
}
Json Document::catalog() const {
    Json commands=Json::array(); for(const auto& [name,c]:commands_) commands.push_back({{"op",name},{"description",c.first}});
    return {{"documentVersion",1},{"primitives",registry_.describe()},{"commands",commands},
        {"coordinates","meters; +x right, +y up; motion uses seconds; screen text size uses pixels"},
        {"motion",{{"vx",0},{"vy",15},{"ax",0},{"ay",-9.8}}},
        {"binding",{{"target","object-id"},{"kind","position or velocity"},{"scale",0.25}}}};
}
void Document::startJournal(const std::string& directory) {
    require(journalPath_.empty(),"Journal is already open");
    const auto folder=std::filesystem::absolute(std::filesystem::u8path(directory));
    std::filesystem::create_directories(folder);
    auto timestamp=utcNow(); std::replace(timestamp.begin(),timestamp.end(),':','-');
    const auto path=folder/std::filesystem::u8path("session-"+timestamp+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+".jsonl");
    require(!std::filesystem::exists(path),"Journal already exists");
    std::ofstream stream(path,std::ios::binary); require(bool(stream),"Cannot create operation journal");
    stream<<Json({{"journalVersion",1},{"kind","session"},{"utc",utcNow()},{"initialState",snapshot()}}).dump()<<'\n';
    stream.flush(); require(bool(stream),"Cannot write journal header"); journalPath_=path.u8string();
}
void Document::record(Json event) {
    event["id"]=nextEvent_++; event["utc"]=utcNow();
    const auto line=event.dump();
    audit_.push_back(event);
    historyBytes_+=line.size(); history_.push_back(std::move(event));
    while(history_.size()>1000 || (historyBytes_>16*1024*1024 && history_.size()>1)) {
        historyBytes_-=history_.front().dump().size(); history_.pop_front();
    }
    if(!journalPath_.empty() && journalError_.empty()) {
        std::ofstream stream(std::filesystem::u8path(journalPath_),std::ios::binary|std::ios::app);
        stream<<line<<'\n'; stream.flush();
        if(!stream) journalError_="磁盘日志写入失败；后续操作仍在内存记录，本次会话的磁盘日志已停止。请保存场景并检查磁盘。";
    }
}
Json Document::history(uint64_t after,size_t limit) const {
    require(limit>=1 && limit<=100,"History page limit must be 1..100");
    Json entries=Json::array(); size_t bytes=0; uint64_t cursor=after;
    for(const auto& event:audit_) if(event.at("id").get<uint64_t>()>after) {
        Json summary=event;
        const auto requestBytes=summary.at("request").dump().size();
        if(requestBytes>12000) { summary["request"]={{"method",event["request"].value("method","invalid")}}; summary["requestOmitted"]=true; summary["requestBytes"]=requestBytes; }
        summary.erase("resolvedDocument");
        const auto count=summary.dump().size(); if(entries.size()>=limit || bytes+count>700000) break;
        bytes+=count; cursor=event["id"]; entries.push_back(std::move(summary));
    }
    return {{"entries",entries},{"nextCursor",cursor},{"hasMore",cursor<nextEvent_-1},
        {"firstAvailableId",audit_.empty()?nextEvent_:uint64_t(1)},
        {"journalPath",journalPath_},{"journalError",journalError_},{"lastId",nextEvent_-1}};
}
Json Document::dispatch(const Json& request,const std::string& source) {
    if(request.value("method","")=="sample_animation") {
        const auto args=request.at("args");const double time=args.contains("time")?number(args,"time"):time_;const auto& o=object(args.at("id"));
        require(time>=0 && time<=scene()["duration"].get<double>(),"Sample time outside duration");
        const auto state=evaluate(o["id"],time);
        return {{"properties",animation::properties(o,time,scene()["duration"])},{"state",{{"x",state.x},{"y",state.y},{"vx",state.vx},{"vy",state.vy}}},
            {"animation",o.contains("animation")?animation::evaluate(o["animation"],time,scene()["duration"]):Json()}};
    }
    if(request.value("method","")=="get_timeline") return timeline();
    if(request.value("method","")=="preview") {
        const auto args=request.value("args",Json::object());
        if(!args.contains("revision") || args["revision"].is_null()) previewRevision_.reset();
        else {
            require(args["revision"].is_number_integer() && args["revision"]>=0,"Preview revision must be non-negative");
            const uint64_t revision=args["revision"];
            require(std::any_of(checkpoints_.begin(),checkpoints_.end(),[&](const Checkpoint& c){return c.revision==revision;}),"Requested checkpoint is no longer retained");
            previewRevision_=revision;
        }
        playing_=false; time_=std::min(time_,scene()["duration"].get<double>()); step_=std::min(step_,scene()["steps"].get<int>()-1); return timeline();
    }
    if(request.value("method","")=="get_history") {
        const auto args=request.value("args",Json::object());
        if(args.contains("after")) require(args["after"].is_number_integer() && args["after"]>=0,"History cursor must be a non-negative integer");
        return history(args.value("after",uint64_t(0)),args.value("limit",size_t(100)));
    }
    const std::string method=request.value("method","");
    if(source=="preview") { require(method=="playback","Preview requests may only update playback"); return executeRequest(request); }
    if(method=="get_scene" || method=="get_catalog") return executeRequest(request);
    require(source=="human" || source=="mcp" || source=="system","Unknown operation source");
    const auto beforeRevision=revision_; const Json beforePlayback=snapshot()["playback"]; relatedEvent_=0;
    Json event={{"source",source},{"request",request},{"beforeRevision",beforeRevision},{"beforePlayback",beforePlayback}};
    if(method=="restore_preview" && previewing()) event["restoredRevision"]=displayRevision();
    Json result;
    try { result=executeRequest(request); }
    catch(const std::exception& e) {
        event["status"]="rejected"; event["error"]=e.what(); event["afterRevision"]=revision_; event["afterPlayback"]=snapshot()["playback"];
        record(std::move(event)); throw;
    }
    event["status"]="success"; event["afterRevision"]=revision_; event["afterPlayback"]=snapshot()["playback"];
    event["documentChanged"]=beforeRevision!=revision_;
    if(beforeRevision!=revision_) checkpoint(nextEvent_);
    if(relatedEvent_) event["relatedEvent"]=relatedEvent_;
    if(method=="load" || method=="restore_preview") event["resolvedDocument"]=scene_;
    record(std::move(event));
    if(!journalError_.empty()) result["journalWarning"]=journalError_;
    return result;
}
Json Document::executeRequest(const Json& request) {
    const auto method=request.at("method").get<std::string>(); const auto args=request.value("args",Json::object());
    if(method=="get_scene") return snapshot();
    if(method=="get_catalog") return catalog();
    require(!previewing() || method=="restore_preview" || method=="playback","Exit history preview or restore it before editing/saving");
    if(request.contains("expectedRevision")) require(request.at("expectedRevision")==revision_,"Revision conflict: query scene and retry consciously");
    if(method=="restore_preview") {
        require(previewing(),"No history state is being previewed"); const Json previous=scene(); commit(previous); previewRevision_.reset();
    } else if(method=="apply") {
        const auto& edits=args.at("commands"); require(edits.is_array() && !edits.empty() && edits.size()<=1000,"commands must contain 1..1000 edits");
        Json next=scene_;
        for(const auto& command:edits) { const auto op=command.at("op").get<std::string>(); const auto it=commands_.find(op); require(it!=commands_.end(),"Unsupported command: "+op); it->second.second(next,command); }
        commit(std::move(next));
    } else if(method=="replace") { commit(args.at("document")); }
    else if(method=="undo" || method=="redo") {
        auto& source=method=="undo"?undo_:redo_; auto& destination=method=="undo"?redo_:undo_;
        require(!source.empty(),"History is empty"); relatedEvent_=source.back().event;
        destination.push_back({scene_,relatedEvent_}); scene_=source.back().scene; source.pop_back(); ++revision_; playing_=false;
        time_=std::min(time_,scene_["duration"].get<double>()); step_=std::min(step_,scene_["steps"].get<int>()-1);
    } else if(method=="playback") {
        double time=args.contains("time")?number(args,"time"):time_;
        double rate=args.contains("rate")?number(args,"rate"):rate_;
        int step=args.value("step",step_); const bool playing=args.value("playing",playing_);
        require(time>=0 && time<=scene()["duration"].get<double>(),"time outside scene duration");
        require(rate>0 && rate<=10,"rate must be in (0,10]"); require(step>=0 && step<scene()["steps"].get<int>(),"step outside scene steps");
        require(!previewing() || !playing,"Playback is paused during history preview");
        time_=time; rate_=rate; step_=step; playing_=playing;
    } else if(method=="save") {
        const auto path=std::filesystem::u8path(args.at("path").get<std::string>());
        require(path.extension()==L".json","Save requires a .json path");
        require(args.value("overwrite",false) || !std::filesystem::exists(path),"File already exists; set overwrite explicitly");
        if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        // Write beside the destination, then atomically replace on Windows.
        auto temporary=path; temporary+=L".writing";
        require(!std::filesystem::exists(temporary),"Temporary save file already exists");
        { std::ofstream stream(temporary,std::ios::binary); require(bool(stream),"Cannot open save path"); stream<<scene_.dump(2); stream.flush(); require(bool(stream),"Failed writing scene"); }
        // Atomic replacement implemented by the platform service, see Windows include below.
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary); throw std::runtime_error("Cannot replace destination scene file");
        }
    } else if(method=="load") {
        const auto path=std::filesystem::u8path(args.at("path").get<std::string>());
        require(std::filesystem::file_size(path)<=900000,"Scene file exceeds 900 KB");
        std::ifstream stream(path,std::ios::binary); require(bool(stream),"Cannot open scene"); Json next; stream>>next; commit(std::move(next));
    } else throw std::runtime_error("Unsupported method: "+method);
    return snapshot();
}
const Json& Document::object(const std::string& id) const {
    for(const auto& o:scene()["objects"]) if(o["id"]==id) return o;
    throw std::runtime_error("Unknown object: "+id);
}
State Document::evaluate(const std::string& id,double time) const {
    const auto& o=object(id); const auto& p=o.at("properties"); State state{p["x"],p["y"],0,0};
    if(o.contains("motion")) { const auto& m=o["motion"]; const double vx=m["vx"],vy=m["vy"],ax=m["ax"],ay=m["ay"];
        state.x+=vx*time+0.5*ax*time*time; state.y+=vy*time+0.5*ay*time*time; state.vx=vx+ax*time; state.vy=vy+ay*time;
    }
    if(o.contains("binding")) { const auto& b=o["binding"]; const auto target=evaluate(b["target"],time); state.x+=target.x; state.y+=target.y; state.vx=target.vx; state.vy=target.vy; }
    if(o.contains("animation")) {
        const auto values=animation::evaluate(o["animation"],time,scene()["duration"])["channels"];
        state.x=values.value("x",state.x);state.y=values.value("y",state.y);state.vx=values.value("vx",0.0);state.vy=values.value("vy",0.0);
    }
    return state;
}
void Document::advance(double dt) {
    if(playing_) { time_=std::min(time_+std::clamp(dt,0.0,0.1)*rate_,scene_["duration"].get<double>()); if(time_>=scene_["duration"].get<double>()) playing_=false; }
}
Json Document::demo() {
    Registry registry;
    Json objects=Json::array();
    auto add=[&](const char* id,const char* type,const char* name,Json props,int step=0) {
        Json p=registry.get(type).defaults; p.update(props);
        objects.push_back({{"id",id},{"type",type},{"name",name},{"properties",p},{"visible",true},{"locked",false},{"step",step}});
    };
    add("ground","ground","地面",{{"x",-3},{"dx",12}});
    add("ball","point","上抛质点",{{"radius",0.22},{"color",{0.12,0.48,0.78,1}}});
    objects.back()["motion"]={{"vx",0},{"vy",15},{"ax",0},{"ay",-9.8}};
    add("initial","arrow","初速度",{{"x",-0.7},{"dx",0},{"dy",3.75},{"color",{0.55,0.6,0.65,1}}},1);
    add("velocity","arrow","瞬时速度",{{"x",0},{"y",0},{"color",{0.12,0.48,0.78,1}}},1);
    objects.back()["binding"]={{"target","ball"},{"kind","velocity"},{"scale",0.25}};
    add("gravity","arrow","重力加速度",{{"x",1.5},{"y",0},{"dx",0},{"dy",-2.45},{"color",{0.55,0.3,0.72,1}}},2);
    objects.back()["binding"]={{"target","ball"},{"kind","position"},{"scale",1}};
    add("title","text","题目",{{"x",4},{"y",11},{"text","竖直上抛 · 忽略空气阻力"},{"size",24}});
    add("known","text","已知条件",{{"x",4},{"y",9.8},{"text","初速度 15 m/s，重力加速度 9.8 m/s²"},{"size",19}});
    add("position","formula","位置公式",{{"x",4},{"y",8},{"formula","position_y"}},2);
    add("speed","formula","速度公式",{{"x",4},{"y",6.3},{"formula","velocity_y"}},2);
    add("apex","text","最高点提示",{{"x",4},{"y",4.4},{"text","最高点：速度为零，加速度仍向下。"}},3);
    add("initial-label","formula","初速度标记",{{"x",-1.9},{"y",2.9},{"formula","initial"}},1);
    add("velocity-label","formula","速度标记",{{"x",-1.3},{"y",0.6},{"formula","velocity"},{"color",{0.12,0.48,0.78,1}}},1);
    objects.back()["binding"]={{"target","ball"},{"kind","position"},{"scale",1}};
    add("gravity-label","formula","重力标记",{{"x",1.9},{"y",-0.8},{"formula","gravity"},{"color",{0.55,0.3,0.72,1}}},2);
    objects.back()["binding"]={{"target","ball"},{"kind","position"},{"scale",1}};
    return {{"version",1},{"title","竖直上抛示意"},{"duration",30.0/9.8},{"steps",4},{"objects",objects}};
}
}
