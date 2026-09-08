#include "editor.hpp"
#include "formula.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace physics_editor {
namespace {
ImU32 color(const Json& p) { const auto& c=p.at("color"); return ImGui::ColorConvertFloat4ToU32({c[0],c[1],c[2],c[3]}); }
float f(const Json& j,const char* key) { return j.at(key).get<float>(); }
float distance(ImVec2 a,ImVec2 b) { return std::hypot(a.x-b.x,a.y-b.y); }
bool inputNumber(const char* label,double* value) {
    char buffer[64]; std::snprintf(buffer,sizeof(buffer),"%.9g",*value);
    if(!ImGui::InputText(label,buffer,sizeof(buffer),ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CharsScientific)) return false;
    char* end=nullptr; const double parsed=std::strtod(buffer,&end);
    if(end==buffer || *end!='\0' || !std::isfinite(parsed)) return false;
    *value=parsed; return true;
}
}
Editor::Editor() {
    initializeRenderers();
    document_.addValidator([](const Json& scene) {
        for(const auto& o:scene["objects"]) if(o["type"]=="formula") {
            const std::string key=o["properties"]["formula"]; bool found=false;
            for(const auto& e:formula_cache::entries) if(key==e.name) found=true;
            if(!found) throw std::runtime_error("Unknown formula asset: "+key+"; query get_formulas");
        }
    });
}
ImVec2 Editor::screen(double x,double y) const { return {origin_.x+static_cast<float>(x)*zoom_,origin_.y-static_cast<float>(y)*zoom_}; }
ImVec2 Editor::world(ImVec2 p) const { return {(p.x-origin_.x)/zoom_,(origin_.y-p.y)/zoom_}; }
void Editor::arrow(ImDrawList* list,ImVec2 a,ImVec2 b,ImU32 tint,float stroke) {
    const float length=distance(a,b); if(length<0.5f) return;
    const float ux=(b.x-a.x)/length, uy=(b.y-a.y)/length,head=std::min(12.0f,length*0.4f);
    list->AddLine(a,b,tint,stroke);
    list->AddTriangleFilled(b,{b.x-head*ux+head*.4f*uy,b.y-head*uy-head*.4f*ux},
        {b.x-head*ux-head*.4f*uy,b.y-head*uy+head*.4f*ux},tint);
}
void Editor::initializeRenderers() {
    renderers_["point"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { d->AddCircleFilled(screen(s.x,s.y),std::max(3.f,f(o["properties"],"radius")*zoom_),c); };
    renderers_["circle"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; d->AddCircle(screen(s.x,s.y),f(p,"radius")*zoom_,c,64,f(p,"stroke")); };
    renderers_["rectangle"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; d->AddRect(screen(s.x,s.y+f(p,"height")),screen(s.x+f(p,"width"),s.y),c,0,0,f(p,"stroke")); };
    renderers_["line"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; d->AddLine(screen(s.x,s.y),screen(s.x+f(p,"dx"),s.y+f(p,"dy")),c,f(p,"stroke")); };
    renderers_["arrow"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; double dx=f(p,"dx"),dy=f(p,"dy");
        if(o.contains("binding") && o["binding"]["kind"]=="velocity") { const double scale=o["binding"]["scale"]; dx=s.vx*scale; dy=s.vy*scale; }
        arrow(d,screen(s.x,s.y),screen(s.x+dx,s.y+dy),c,f(p,"stroke"));
    };
    renderers_["ground"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; const auto a=screen(s.x,s.y), b=screen(s.x+f(p,"dx"),s.y+f(p,"dy"));
        d->AddLine(a,b,c,f(p,"stroke")); const float length=distance(a,b); if(length<1) return;
        const float ux=(b.x-a.x)/length,uy=(b.y-a.y)/length;
        for(float t=15;t<length && t<5000;t+=24) { ImVec2 q{a.x+ux*t,a.y+uy*t}; d->AddLine(q,{q.x-8*ux-12*uy,q.y-8*uy+12*ux},c,1.4f); }
    };
    renderers_["text"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; d->AddText(ImGui::GetFont(),f(p,"size"),screen(s.x,s.y),c,p["text"].get_ref<const std::string&>().c_str()); };
    renderers_["formula"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; try { math_text::draw(d,screen(s.x,s.y),p["formula"].get_ref<const std::string&>().c_str(),c,f(p,"scale")); }
        catch(const std::exception&) { d->AddText(screen(s.x,s.y),IM_COL32(200,30,30,255),"未知公式资源"); }
    };
    renderers_["polyline"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; const auto& points=p["points"];
        for(size_t i=1;i<points.size();++i) d->AddLine(screen(s.x+points[i-1][0].get<double>(),s.y+points[i-1][1].get<double>()),screen(s.x+points[i][0].get<double>(),s.y+points[i][1].get<double>()),c,f(p,"stroke"));
    };
}
void Editor::windowState(bool visible,bool minimized,unsigned frames) {
    viewState_["visible"]=visible; viewState_["minimized"]=minimized; viewState_["presentedFrames"]=frames;
}
void Editor::presented() { viewState_["presentedRevision"]=drawnRevision_; viewState_["presentedTime"]=drawnTime_; }
Json Editor::dispatch(const Json& request,const std::string& source) {
    const std::string method=request.at("method");
    if(method=="get_view_state") return viewState_;
    if(method=="show_editor") { showRequested_=true; return {{"showRequested",true}}; }
    if(source=="mcp" && followMcp_ && (method=="apply" || method=="replace" || method=="load" || method=="undo" || method=="redo" || method=="playback")) showRequested_=true;
    // Formula assets are part of this renderer's capabilities. Core remains GPU/TeX independent.
    if(request.at("method")=="get_formulas") {
        Json result=Json::array(); for(const auto& e:formula_cache::entries) result.push_back(e.name);
        return {{"assets",result},{"note","Precompiled LaTeX assets. To add expressions, edit formulas.json and regenerate the cache before rebuilding."}};
    }
    auto result=document_.dispatch(request,source);
    if(method=="get_history" || method=="get_scene" || method=="get_catalog") return result;
    if(method=="apply" || method=="replace" || method=="load" || method=="undo" || method=="redo") dirty_=true;
    if(method=="save" || method=="load") {
        dirty_=false;
        const auto path=request.at("args").at("path").get<std::string>();
        if(path.size()<sizeof(path_)) strcpy_s(path_,path.c_str());
    }
    status_="完成："+method+" · 修订 "+std::to_string(document_.revision());
    if(!document_.journalError().empty()) status_+=" · "+document_.journalError();
    return result;
}
void Editor::attempt(const Json& request) { try { dispatch(request); } catch(const std::exception& e) { status_=std::string("操作失败：")+e.what(); } }
bool Editor::requestClose() { closeDialog_=dirty_; return !dirty_; }
void Editor::execute(Json commands) { attempt({{"method","apply"},{"args",{{"commands",commands}}}}); }
void Editor::palette() {
    ImGui::TextUnformatted("添加图元");
    for(const auto& p:document_.registry().describe()) {
        const auto type=p["type"].get<std::string>();
        if(ImGui::Button(p["label"].get_ref<const std::string&>().c_str(),{94,30})) {
            std::string id;
            do { id=type+"_"+std::to_string(nextId_++); } while(std::any_of(document_.scene()["objects"].begin(),document_.scene()["objects"].end(),[&](const Json& o){return o["id"]==id;}));
            execute(Json::array({{{"op","create"},{"id",id},{"type",type},{"name",p["label"]},{"properties",{{"x",3},{"y",5}}}}})); selected_=id;
        }
        if(type=="circle") { const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax(); circleButton_={(a.x+b.x)/2,(a.y+b.y)/2}; }
        if(type=="circle" || type=="ground" || type=="point" || type=="rectangle") ImGui::SameLine();
    }
    ImGui::Separator(); ImGui::TextUnformatted("场景对象（后面的覆盖前面的）");
    std::string remove;
    for(const auto& o:document_.scene()["objects"]) {
        const auto id=o["id"].get<std::string>(); ImGui::PushID(id.c_str());
        const std::string label=(o["visible"].get<bool>()?"":"[隐藏] ")+o["name"].get<std::string>();
        if(ImGui::Selectable(label.c_str(),selected_==id)) selected_=id;
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextWrapped("左键选择 / 拖动原点\n中键平移 · 滚轮缩放\n拖动时暂停动画\nCtrl+Z / Ctrl+Y 撤销重做");
}
void Editor::inspector() {
    ImGui::TextUnformatted("属性");
    const Json* object=nullptr; for(const auto& o:document_.scene()["objects"]) if(o["id"]==selected_) object=&o;
    if(!object) { ImGui::TextWrapped("从画布或左侧选择一个对象。"); return; }
    // Work on a copy: commands may replace the entire document on commit.
    const Json o=*object; const Json p=o["properties"];
    ImGui::Text("ID: %s",selected_.c_str()); ImGui::Text("类型: %s",o["type"].get_ref<const std::string&>().c_str());
    auto patch=[&](Json value) { execute(Json::array({{{"op","update"},{"id",selected_},{"patch",value}}})); };
    char name[512]; strcpy_s(name,o["name"].get_ref<const std::string&>().c_str());
    if(ImGui::InputText("名称",name,sizeof(name),ImGuiInputTextFlags_EnterReturnsTrue)) patch({{"name",name}});
    bool visible=o["visible"], locked=o["locked"];
    if(ImGui::Checkbox("显示",&visible)) patch({{"visible",visible}}); ImGui::SameLine();
    if(ImGui::Checkbox("锁定拖动",&locked)) patch({{"locked",locked}});
    int step=o["step"]; if(ImGui::SliderInt("出现步骤",&step,0,document_.scene()["steps"].get<int>()-1)) patch({{"step",step}});
    ImGui::SeparatorText("位置与形状");
    for(const char* key:{"x","y","dx","dy","radius","width","height","stroke","size","scale"}) if(p.contains(key)) {
        double value=p[key]; if(inputNumber(key,&value)) patch({{"properties",{{key,value}}}});
    }
    float c[4]={p["color"][0],p["color"][1],p["color"][2],p["color"][3]};
    if(ImGui::ColorEdit4("颜色",c,ImGuiColorEditFlags_NoInputs)) patch({{"properties",{{"color",{c[0],c[1],c[2],c[3]}}}}});
    if(p.contains("text")) { char text[8192]; strcpy_s(text,p["text"].get_ref<const std::string&>().c_str());
        if(ImGui::InputText("文字",text,sizeof(text),ImGuiInputTextFlags_EnterReturnsTrue)) patch({{"properties",{{"text",text}}}});
    }
    if(p.contains("formula")) {
        if(ImGui::BeginCombo("公式资源",p["formula"].get_ref<const std::string&>().c_str())) {
            for(const auto& entry:formula_cache::entries) if(ImGui::Selectable(entry.name,p["formula"]==entry.name)) patch({{"properties",{{"formula",entry.name}}}});
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("使用预编译 LaTeX 公式库；新增表达式见公式文档。");
    }
    ImGui::SeparatorText("运动与绑定");
    if(!o.contains("motion") && !o.contains("binding")) {
        if(ImGui::Button("添加匀加速运动")) patch({{"motion",{{"vx",0},{"vy",0},{"ax",0},{"ay",-9.8}}}});
    }
    if(o.contains("motion")) {
        for(const char* key:{"vx","vy","ax","ay"}) { double value=o["motion"][key];
            if(inputNumber(key,&value)) patch({{"motion",{{key,value}}}});
        }
        if(ImGui::Button("移除运动")) patch({{"motion",nullptr}});
    }
    if(!o.contains("motion")) {
        std::string target=o.contains("binding")?o["binding"]["target"].get<std::string>():"无";
        if(ImGui::BeginCombo("绑定对象",target.c_str())) {
            if(ImGui::Selectable("无",target=="无")) patch({{"binding",nullptr}});
            for(const auto& other:document_.scene()["objects"]) if(other["id"]!=selected_ && !other.contains("binding")) {
                if(ImGui::Selectable(other["id"].get_ref<const std::string&>().c_str(),other["id"]==target))
                    patch({{"binding",{{"target",other["id"]},{"kind","position"},{"scale",0.25}}}});
            }
            ImGui::EndCombo();
        }
        if(o.contains("binding") && o["type"]=="arrow") {
            bool velocity=o["binding"]["kind"]=="velocity";
            if(ImGui::Checkbox("箭头表示目标速度",&velocity)) patch({{"binding",{{"kind",velocity?"velocity":"position"}}}});
            double scale=o["binding"]["scale"]; if(inputNumber("矢量比例",&scale)) patch({{"binding",{{"scale",scale}}}});
        }
    }
    if(ImGui::Button("删除对象")) execute(Json::array({{{"op","delete"},{"id",selected_}}}));
    ImGui::SameLine(); if(ImGui::Button("置于最上层")) execute(Json::array({{{"op","reorder"},{"id",selected_},{"index",document_.scene()["objects"].size()-1}}}));
    if(ImGui::CollapsingHeader("完整属性 JSON / 扩展字段")) {
        if(editedId_!=selected_ || editedRevision_!=document_.revision()) {
            Json editable=o; editable.erase("id"); editable.erase("type"); const auto text=editable.dump(2);
            if(text.size()<sizeof(jsonEdit_)) strcpy_s(jsonEdit_,text.c_str()); else strcpy_s(jsonEdit_,"{}");
            editedId_=selected_; editedRevision_=document_.revision();
        }
        ImGui::InputTextMultiline("##json",jsonEdit_,sizeof(jsonEdit_),{-1,230});
        if(ImGui::Button("应用 JSON 属性")) { try { patch(Json::parse(jsonEdit_)); } catch(const std::exception& e) { status_=e.what(); } }
    }
    ImGui::TextWrapped("数值 / 文字输入后按 Enter 提交。x、y 是初始位置；绑定时是相对偏移。");
}
void Editor::canvas(ImVec2 size) {
    drawnRevision_=document_.revision(); drawnTime_=document_.time();
    const auto start=ImGui::GetCursorScreenPos(); size.x=std::max(size.x,100.f); size.y=std::max(size.y,100.f);
    ImGui::InvisibleButton("canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered=ImGui::IsItemHovered(); auto& io=ImGui::GetIO(); auto* draw=ImGui::GetWindowDrawList();
    origin_={start.x+size.x*.25f+pan_.x,start.y+size.y*.85f+pan_.y};
    if(hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) { pan_.x+=io.MouseDelta.x; pan_.y+=io.MouseDelta.y; origin_.x+=io.MouseDelta.x; origin_.y+=io.MouseDelta.y; }
    if(hovered && io.MouseWheel!=0) { const auto before=world(io.MousePos); zoom_=std::clamp(zoom_*std::pow(1.1f,io.MouseWheel),8.f,160.f);
        const auto after=screen(before.x,before.y); pan_.x+=io.MousePos.x-after.x; pan_.y+=io.MousePos.y-after.y; origin_.x+=io.MousePos.x-after.x; origin_.y+=io.MousePos.y-after.y;
    }
    draw->AddRectFilled(start,{start.x+size.x,start.y+size.y},IM_COL32(252,251,248,255),8);
    draw->PushClipRect(start,{start.x+size.x,start.y+size.y},true);
    if(grid_) {
        for(float x=std::fmod(origin_.x-start.x,zoom_);x<size.x;x+=zoom_) draw->AddLine({start.x+x,start.y},{start.x+x,start.y+size.y},IM_COL32(228,231,233,255));
        for(float y=std::fmod(origin_.y-start.y,zoom_);y<size.y;y+=zoom_) draw->AddLine({start.x,start.y+y},{start.x+size.x,start.y+y},IM_COL32(228,231,233,255));
    }
    std::string hit;
    for(const auto& o:document_.scene()["objects"]) {
        if(!o["visible"].get<bool>() || o["step"].get<int>()>document_.step()) continue;
        const auto id=o["id"].get<std::string>(); auto state=document_.evaluate(id,document_.time());
        if(dragging_ && id==selected_) { state.x+=dragPreview_.x; state.y+=dragPreview_.y; }
        if(trails_ && o.contains("motion")) {
            ImVec2 previous=screen(document_.evaluate(id,0).x,document_.evaluate(id,0).y);
            for(int i=1;i<=100;++i) { const auto v=document_.evaluate(id,document_.time()*i/100.0); const auto q=screen(v.x,v.y); draw->AddLine(previous,q,IM_COL32(153,180,197,140),1.5f); previous=q; }
        }
        const auto renderer=renderers_.find(o["type"].get<std::string>());
        if(renderer!=renderers_.end()) renderer->second(draw,o,state,color(o["properties"]));
        const auto anchor=screen(state.x,state.y);
        // Every primitive exposes the same draggable anchor, including text/formula.
        bool contains=distance(io.MousePos,anchor)<16;
        const auto& p=o["properties"]; const std::string type=o["type"];
        if(type=="circle") contains=contains || std::abs(distance(io.MousePos,anchor)-f(p,"radius")*zoom_)<8;
        if(type=="point") contains=contains || distance(io.MousePos,anchor)<f(p,"radius")*zoom_+5;
        if(type=="text" || type=="formula" || type=="rectangle") {
            ImVec2 extent{}; ImVec2 top=anchor;
            if(type=="text") { extent=ImGui::GetFont()->CalcTextSizeA(f(p,"size"),1e6f,0,p["text"].get_ref<const std::string&>().c_str()); }
            else if(type=="formula") extent=math_text::size(p["formula"].get_ref<const std::string&>().c_str(),f(p,"scale"));
            else { extent={f(p,"width")*zoom_,f(p,"height")*zoom_}; top.y-=extent.y; }
            contains=contains || (io.MousePos.x>=top.x && io.MousePos.x<=top.x+extent.x && io.MousePos.y>=top.y && io.MousePos.y<=top.y+extent.y);
        }
        auto nearLine=[&](ImVec2 a,ImVec2 b) {
            const float dx=b.x-a.x,dy=b.y-a.y,length2=dx*dx+dy*dy;
            const float t=length2>0?std::clamp(((io.MousePos.x-a.x)*dx+(io.MousePos.y-a.y)*dy)/length2,0.f,1.f):0;
            return distance(io.MousePos,{a.x+t*dx,a.y+t*dy})<8;
        };
        if(p.contains("dx")) {
            double dx=f(p,"dx"),dy=f(p,"dy");
            if(o.contains("binding") && o["binding"]["kind"]=="velocity") { dx=state.vx*o["binding"]["scale"].get<double>(); dy=state.vy*o["binding"]["scale"].get<double>(); }
            contains=contains || nearLine(anchor,screen(state.x+dx,state.y+dy));
        }
        if(type=="polyline") { const auto& points=p["points"]; for(size_t i=1;i<points.size();++i)
            contains=contains || nearLine(screen(state.x+points[i-1][0].get<double>(),state.y+points[i-1][1].get<double>()),screen(state.x+points[i][0].get<double>(),state.y+points[i][1].get<double>())); }
        if(contains) hit=id;
        if(id==selected_) { draw->AddCircle(anchor,12,IM_COL32(237,149,48,255),24,2); draw->AddText({anchor.x+16,anchor.y+10},IM_COL32(163,99,25,255),o["name"].get_ref<const std::string&>().c_str()); }
    }
    draw->AddText({start.x+16,start.y+12},IM_COL32(105,118,129,255),"运动示意画布 · +y 向上 · 箭头使用独立比例");
    if(hovered && ImGui::IsMouseClicked(0)) {
        if(!hit.empty()) { selected_=hit; const auto& o=document_.object(hit);
            if(!o["locked"].get<bool>()) { if(document_.playing()) document_.dispatch({{"method","playback"},{"args",{{"playing",false}}}},"human"); dragging_=true; dragStart_=io.MousePos; dragPreview_={}; dragRevision_=document_.revision(); }
        } else selected_.clear();
    }
    if(dragging_) {
        if(document_.revision()!=dragRevision_) { dragging_=false; status_="拖动已取消：场景被其他操作更新。"; }
        else {
            dragPreview_={(io.MousePos.x-dragStart_.x)/zoom_,(dragStart_.y-io.MousePos.y)/zoom_};
            if(ImGui::IsMouseReleased(0)) { dragging_=false; if(distance(dragPreview_,{})>.001f) execute(Json::array({{{"op","move"},{"id",selected_},{"dx",dragPreview_.x},{"dy",dragPreview_.y}}})); }
        }
    }
    draw->PopClipRect();
}
void Editor::draw(float dt) {
    document_.advance(dt); auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("物理示意编辑器",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("物理示意编辑器"); ImGui::SameLine(); ImGui::TextDisabled("C++ / Vulkan    %s%s",document_.scene()["title"].get_ref<const std::string&>().c_str(),dirty_?" *":"");
    if(ImGui::Button("撤销")) attempt({{"method","undo"}}); ImGui::SameLine();
    if(ImGui::Button("重做")) attempt({{"method","redo"}}); ImGui::SameLine();
    ImGui::SetNextItemWidth(390); ImGui::InputText("##path",path_,sizeof(path_)); ImGui::SameLine();
    if(ImGui::Button("保存")) attempt({{"method","save"},{"args",{{"path",path_},{"overwrite",true}}}}); ImGui::SameLine();
    if(ImGui::Button("打开")) attempt({{"method","load"},{"args",{{"path",path_}}}}); ImGui::SameLine();
    if(ImGui::Button("上抛示例")) attempt({{"method","replace"},{"args",{{"document",Document::demo()}}}}); ImGui::SameLine();
    if(ImGui::Button("空白")) { auto empty=Document::demo(); empty["objects"]=Json::array(); empty["title"]="未命名"; attempt({{"method","replace"},{"args",{{"document",empty}}}}); }
    ImGui::SameLine(); if(ImGui::Button("操作历史")) historyOpen_=!historyOpen_;
    ImGui::SameLine(); ImGui::Checkbox("跟随 MCP 显示",&followMcp_);
    auto& io=ImGui::GetIO(); if(!io.WantTextInput && io.KeyCtrl) {
        if(ImGui::IsKeyPressed(ImGuiKey_Z,false)) attempt({{"method","undo"}});
        if(ImGui::IsKeyPressed(ImGuiKey_Y,false)) attempt({{"method","redo"}});
        if(ImGui::IsKeyPressed(ImGuiKey_S,false)) attempt({{"method","save"},{"args",{{"path",path_},{"overwrite",true}}}});
    }
    ImGui::Separator();
    const float height=ImGui::GetContentRegionAvail().y-136;
    ImGui::BeginChild("palette",{224,height},ImGuiChildFlags_Borders); palette(); ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("drawing",{ImGui::GetContentRegionAvail().x-340,height}); canvas(ImGui::GetContentRegionAvail()); ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("properties",{0,height},ImGuiChildFlags_Borders); inspector(); ImGui::EndChild();
    if(ImGui::Button(document_.playing()?"暂停":"播放")) attempt({{"method","playback"},{"args",{{"playing",!document_.playing()},{"time",document_.time()>=document_.scene()["duration"].get<double>()?0:document_.time()}}}}); ImGui::SameLine();
    if(ImGui::Button("回到开始")) attempt({{"method","playback"},{"args",{{"time",0},{"playing",false}}}}); ImGui::SameLine();
    float time=static_cast<float>(document_.time()); ImGui::SetNextItemWidth(360);
    if(ImGui::SliderFloat("时间 / s",&time,0,document_.scene()["duration"].get<float>(),"%.3f")) attempt({{"method","playback"},{"args",{{"time",std::min(double(time),document_.scene()["duration"].get<double>())},{"playing",false}}}});
    ImGui::SameLine(); int step=document_.step(); ImGui::SetNextItemWidth(140);
    if(ImGui::SliderInt("解题步骤",&step,0,document_.scene()["steps"].get<int>()-1)) attempt({{"method","playback"},{"args",{{"step",step}}}});
    double duration=document_.scene()["duration"]; ImGui::SetNextItemWidth(130);
    if(inputNumber("总时长 / s",&duration)) execute(Json::array({{{"op","settings"},{"duration",duration}}})); ImGui::SameLine();
    ImGui::Checkbox("网格",&grid_); ImGui::SameLine(); ImGui::Checkbox("轨迹",&trails_); ImGui::SameLine();
    if(ImGui::Button("复位视图")) { pan_={}; zoom_=35; }
    ImGui::SameLine(); double steps=document_.scene()["steps"].get<double>(); ImGui::SetNextItemWidth(70);
    if(inputNumber("步骤数",&steps) && steps==std::floor(steps) && steps>=1 && steps<=100) execute(Json::array({{{"op","settings"},{"steps",static_cast<int>(steps)}}}));
    ImGui::TextWrapped("%s",status_.c_str());
    if(closeDialog_) { ImGui::OpenPopup("保存修改？"); closeDialog_=false; }
    if(ImGui::BeginPopupModal("保存修改？",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("场景有未保存的修改。");
        if(ImGui::Button("保存并退出")) {
            attempt({{"method","save"},{"args",{{"path",path_},{"overwrite",true}}}});
            if(!dirty_) { quit_=true; ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine(); if(ImGui::Button("不保存退出")) { quit_=true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if(ImGui::Button("取消")) { smokeCloseCancelled_=true; ImGui::CloseCurrentPopup(); }
        const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax(); closeCancelCenter_={(a.x+b.x)/2,(a.y+b.y)/2};
        ImGui::TextWrapped("%s",status_.c_str());
        ImGui::EndPopup();
    }
    ImGui::End();
    if(historyOpen_) historyPanel();
}
void Editor::historyPanel() {
    ImGui::SetNextWindowSize({940,580},ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("统一操作历史",&historyOpen_)) { ImGui::End(); return; }
    const auto page=document_.history(historyAfter_,100);
    ImGui::TextWrapped("人和 MCP 共用编辑命令与撤销栈；查询、鼠标移动和自动播放帧不计入操作日志。");
    ImGui::TextWrapped("会话日志：%s",page["journalPath"].get_ref<const std::string&>().c_str());
    if(!page["journalError"].get_ref<const std::string&>().empty()) ImGui::TextColored({1,.45f,.3f,1},"%s",page["journalError"].get_ref<const std::string&>().c_str());
    if(ImGui::Button("最早记录")) historyAfter_=0; ImGui::SameLine();
    if(ImGui::Button("最近记录")) { const auto last=page["lastId"].get<uint64_t>(); historyAfter_=last>100?last-100:0; }
    ImGui::SameLine(); if(ImGui::Button("下一页") && page["hasMore"].get<bool>()) historyAfter_=page["nextCursor"];
    if(ImGui::BeginTable("events",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,{0,260})) {
        for(const char* label:{"序号","来源","操作","结果 / 修订","时间 UTC"}) ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        for(const auto& e:page["entries"]) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto id=std::to_string(e["id"].get<uint64_t>());
            if(ImGui::Selectable(id.c_str(),!historySelection_.is_null() && historySelection_["id"]==e["id"],ImGuiSelectableFlags_SpanAllColumns)) historySelection_=e;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e["source"]=="human"?"人类界面":e["source"]=="mcp"?"MCP":"系统");
            ImGui::TableNextColumn(); const auto& r=e["request"];
            ImGui::TextUnformatted(r.value("method","invalid").c_str());
            if(r.contains("args") && r["args"].contains("commands")) for(const auto& c:r["args"]["commands"]) if(c.is_object()) {
                const auto op=c.contains("op")?c["op"].dump():"?", targetId=c.contains("id")?c["id"].dump():"";
                ImGui::Text("  %s  %s",op.c_str(),targetId.c_str());
            }
            ImGui::TableNextColumn(); ImGui::Text("%s / %llu",e["status"]=="success"?"完成":"拒绝",e["afterRevision"].get<unsigned long long>());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e["utc"].get_ref<const std::string&>().c_str());
        }
        ImGui::EndTable();
    }
    ImGui::TextUnformatted("选择一条记录查看命令参数、结果和撤销关联：");
    ImGui::BeginChild("event-details",{0,0});
    if(!historySelection_.is_null()) ImGui::TextUnformatted(historySelection_.dump(2).c_str());
    ImGui::EndChild(); ImGui::End();
}
void Editor::smokeScenario(int scenario) {
    if(scenario==0) dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","create"},{"id","smoke-circle"},{"type","circle"}}})}}}});
    if(scenario==1) dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","move"},{"id","smoke-circle"},{"dx",2},{"dy",4}}})}}}});
    if(scenario==2) dispatch({{"method","undo"}});
    if(scenario==3) dispatch({{"method","redo"}});
    if(scenario==4) dispatch({{"method","playback"},{"args",{{"time",1.53},{"step",3}}}});
}
void Editor::smokeInput(unsigned frame) {
    auto& io=ImGui::GetIO();
    if(frame==3 || frame==4) { io.AddMousePosEvent(circleButton_.x,circleButton_.y); io.AddMouseButtonEvent(0,frame==3); }
    if(frame==8) { smokeCreated_=selected_; auto p=screen(3,5); io.AddMousePosEvent(p.x,p.y); io.AddMouseButtonEvent(0,true); }
    if(frame==10) { auto p=screen(4,5); io.AddMousePosEvent(p.x,p.y); }
    if(frame==12) io.AddMouseButtonEvent(0,false);
    if(frame==21 || frame==22) { io.AddMousePosEvent(closeCancelCenter_.x,closeCancelCenter_.y); io.AddMouseButtonEvent(0,frame==21); }
    if(frame==27 || frame==28) { io.AddKeyEvent(ImGuiMod_Ctrl,frame==27); io.AddKeyEvent(ImGuiKey_Z,frame==27); }
}
void Editor::smokeAfterFrame(unsigned frame) {
    if(frame==18 && (smokeCreated_.empty() || std::abs(document_.object(smokeCreated_)["properties"]["x"].get<double>()-4)>0.001))
        throw std::runtime_error("Palette click / canvas drag input test failed");
    if(frame==18 && requestClose()) throw std::runtime_error("Unsaved close did not request confirmation");
    if(frame==25 && !smokeCloseCancelled_) throw std::runtime_error("Unsaved close cancel input failed");
    if(frame==26) dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","move"},{"id",smokeCreated_},{"dx",2},{"dy",0}}})}}}},"mcp");
    if(frame==29) {
        if(std::abs(document_.object(smokeCreated_)["properties"]["x"].get<double>()-4)>0.001) throw std::runtime_error("Human Ctrl+Z did not undo MCP edit");
        const auto events=document_.history()["entries"];
        if(events.back()["source"]!="human" || !events.back().contains("relatedEvent")) throw std::runtime_error("Shared command history lacks cross-source undo link");
        historyOpen_=true;
    }
    if(frame>=72 && frame<=104 && (frame-72)%8==0) smokeScenario((frame-72)/8);
    if(frame==110) smokeCheck();
}
void Editor::smokeCheck() {
    if(document_.object("smoke-circle")["properties"]["x"]!=2) throw std::runtime_error("Editor history smoke check failed");
    if(document_.evaluate("ball",15.0/9.8).vy>1e-10) throw std::runtime_error("Editor motion smoke check failed");
}
void Editor::captureFrame(unsigned frame,unsigned count) {
    document_.dispatch({{"method","playback"},{"args",{{"time",document_.scene()["duration"].get<double>()*(double(frame)/(count-1))},{"playing",false},{"step",3}}}},"preview");
}
}
