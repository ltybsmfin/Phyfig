#include "editor.hpp"
#include "component.hpp"
#include "formula.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cfloat>
#include <cstdio>
#include <cstdlib>

namespace physics_editor {
void Editor::switchScope(const std::string& scope) {
    workspace_.document(scope); if(scope_==scope) return;
    cancelColorEdit("颜色编辑已取消：范围已切换。"); cancelStepEdit("出现步骤编辑已取消：范围已切换。");
    strcpy_s(scope_=="scene"?scenePath_:componentPath_,sizeof(path_),path_);
    scope_=scope; strcpy_s(path_,scope_=="scene"?scenePath_:componentPath_);
    selected_.clear(); grouped_.clear(); dragging_=false; keyDragging_=false;selectedKey_=-1;timeSeeking_=false; editedId_.clear(); editedRevision_=~uint64_t(0);
    metadataRevision_=~uint64_t(0); historyAfter_=0; historySelection_=nullptr; fitRequested_=true;
}
void Editor::componentPanel() {
    ImGui::SeparatorText(scope_=="scene"?"可复用组件":"组件定义草稿");
    if(scope_=="scene") {
        if(!workspace_.hasComponent()) {
            ImGui::SetNextItemWidth(-1); ImGui::InputText("##component-id",componentId_,sizeof(componentId_));
            if(ImGui::Button(grouped_.empty()?"新建空白组件":"用勾选图元建立组件")) {
                Json ids=Json::array(); for(const auto& id:grouped_) ids.push_back(id);
                attempt({{"method","open_component"},{"scope","scene"},{"args",{{"id",componentId_},{"objects",ids}}}});
            }
            ImGui::TextWrapped("勾选左侧图元后建立定义草稿。原图元保留，发布后可插入多个实例。");
        }
        const auto definitions=active().scene().value("components",Json::object());
        for(auto it=definitions.begin();it!=definitions.end();++it) {
            ImGui::PushID(it.key().c_str()); ImGui::TextUnformatted(it.key().c_str());
            if(ImGui::SmallButton("插入实例")) {
                std::string id; do { id="instance_"+std::to_string(nextId_++); } while(std::any_of(active().scene()["objects"].begin(),active().scene()["objects"].end(),[&](const Json& o){return o["id"]==id;}));
                execute(Json::array({{{"op","create"},{"id",id},{"type","component"},{"name",it.value()["title"]},{"properties",{{"definition",it.key()}}}}}));
                selected_=id; fitRequested_=true;
            }
            ImGui::SameLine(); ImGui::BeginDisabled(workspace_.hasComponent());
            if(ImGui::SmallButton("编辑定义")) attempt({{"method","open_component"},{"scope","scene"},{"args",{{"id",it.key()}}}});
            ImGui::EndDisabled(); ImGui::PopID();
        }
    } else {
        const auto meta=active().scene()["componentDefinition"];
        ImGui::Text("%s",meta["id"].get_ref<const std::string&>().c_str());
        if(ImGui::Button("发布到主场景")) attempt({{"method","publish_component"},{"expectedRevision",document().revision()},{"args",{{"componentRevision",active().revision()}}}});
        ImGui::TextWrapped("发布产生一次主场景操作。草稿内部修改只进入本页历史。");
        for(auto it=meta["parameters"].begin();it!=meta["parameters"].end();++it) {
            ImGui::PushID(it.key().c_str());
            if(ImGui::TreeNodeEx(it.key().c_str(),ImGuiTreeNodeFlags_DefaultOpen)) {
                for(const char* key:{"default","min","max"}) {
                    double value=it.value()[key]; ImGui::SetNextItemWidth(170);
                    char buffer[64]; std::snprintf(buffer,sizeof(buffer),"%.9g",value);
                    const char* label=std::strcmp(key,"default")==0?"默认值":std::strcmp(key,"min")==0?"最小值":"最大值";
                    if(ImGui::InputText(label,buffer,sizeof(buffer),ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CharsScientific)) {
                        char* end=nullptr; value=std::strtod(buffer,&end);
                        if(end!=buffer && *end=='\0' && std::isfinite(value))
                            execute(Json::array({{{"op","component_settings"},{"patch",{{"parameters",{{it.key(),{{key,value}}}}}}}}}));
                    }
                }
                ImGui::Text("单位：%s",it.value().value("unit",std::string()).c_str()); ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if(ImGui::CollapsingHeader("高级：参数 / 绑定表达式 JSON")) {
            if(metadataRevision_!=active().displayRevision()) {
                const auto text=meta.dump(2); if(text.size()<sizeof(metadata_)) strcpy_s(metadata_,text.c_str());
                else strcpy_s(metadata_,"{}"); metadataRevision_=active().displayRevision();
            }
            ImGui::InputTextMultiline("##metadata",metadata_,sizeof(metadata_),{-1,240});
            if(ImGui::Button("应用参数定义")) {
                try { execute(Json::array({{{"op","component_settings"},{"patch",Json::parse(metadata_)}}})); }
                catch(const std::exception& e) { status_=e.what(); }
            }
            ImGui::TextWrapped("数值属性旁的‘公开’建立参数。min/max 定义范围；表达式 sin/cos 使用角度制。绑定的属性由参数计算。");
        }
    }
}
namespace {
std::pair<ImVec2,ImVec2> boundsFor(const Document& doc,const Json& o,State state,float scale,float zoom) {
    const auto& p=o["properties"]; const std::string type=o["type"];
    ImVec2 low{FLT_MAX,FLT_MAX},high{-FLT_MAX,-FLT_MAX};
    auto include=[&](double x,double y){low.x=std::min(low.x,float(x));low.y=std::min(low.y,float(y));high.x=std::max(high.x,float(x));high.y=std::max(high.y,float(y));};
    include(state.x,state.y);
    if(type=="component") {
        for(const auto& child:components::instantiate(doc.scene(),o,state)) {
            const auto b=boundsFor(doc,child,{child["properties"]["x"],child["properties"]["y"],0,0},scale,zoom); include(b.first.x,b.first.y); include(b.second.x,b.second.y);
        }
    } else if(type=="arc" || type=="circle" || type=="point") {
        const double start=type=="arc"?p["start"].get<double>():0,end=type=="arc"?p["end"].get<double>():360,r=p["radius"];
        for(int i=0;i<=64;++i) { const double a=(start+(end-start)*i/64)*3.141592653589793/180; include(state.x+r*std::cos(a),state.y+r*std::sin(a)); }
    } else if(type=="rectangle") {
        const double a=p.value("rotation",0.0)*3.141592653589793/180,c=std::cos(a),s=std::sin(a),w=p["width"],h=p["height"];
        for(double x:{0.0,w}) for(double y:{0.0,h}) include(state.x+c*x-s*y,state.y+s*x+c*y);
    } else if(type=="text" || type=="formula") {
        ImVec2 extent{};
        if(type=="text") {
            const float textSize=p["size"].get<float>()*scale;
            const auto& content=p["text"].get_ref<const std::string&>();
            ImFont* font=ImGui::GetFont();
            extent=font?font->CalcTextSizeA(textSize,1e6f,0,content.c_str())
                :ImVec2{float(content.size())*textSize*.6f,textSize};
        } else extent=math_text::size(p["formula"].get_ref<const std::string&>().c_str(),p["scale"].get<float>()*scale);
        include(state.x+extent.x/zoom,state.y-extent.y/zoom);
    } else if(type=="polyline") for(const auto& point:p["points"]) include(state.x+point[0].get<double>(),state.y+point[1].get<double>());
    if(p.contains("dx")) include(state.x+p["dx"].get<double>(),state.y+p["dy"].get<double>());
    return {low,high};
}
}
std::pair<ImVec2,ImVec2> Editor::bounds(const Json& o,State state) { return boundsFor(active(),o,state,canvasScale(),zoom_); }
Editor::CanvasView Editor::fitView(const std::string& scope,float width,float height) {
    Document& doc=workspace_.document(scope);
    const Json objects=scope=="component"?components::materialize(doc.scene()):doc.scene()["objects"];
    const auto recorded=recordedView(scope);
    float reference=recorded.ready && recorded.referenceZoom>0?recorded.referenceZoom
        :(scope_==scope && referenceZoom_>0?referenceZoom_:0.f);
    float zoom=reference>0?reference:35.f,panX=0,panY=0;
    float originX=width*.25f,originY=height*.85f;
    if(!objects.empty()) for(int pass=0;pass<4;++pass) {
        ImVec2 low{FLT_MAX,FLT_MAX},high{-FLT_MAX,-FLT_MAX};
        for(const auto& o:objects) {
            const auto b=boundsFor(doc,o,{o["properties"]["x"],o["properties"]["y"],0,0},reference>0?zoom/reference:1.f,zoom);
            low.x=std::min(low.x,b.first.x);low.y=std::min(low.y,b.first.y);high.x=std::max(high.x,b.second.x);high.y=std::max(high.y,b.second.y);
        }
        zoom=std::clamp(std::min((width-120)/std::max(.1f,high.x-low.x),(height-110)/std::max(.1f,high.y-low.y)),8.f,1200.f);
        panX=width*.25f-(low.x+high.x)*.5f*zoom; panY=-height*.35f+(low.y+high.y)*.5f*zoom;
        originX=width*.25f+panX; originY=height*.85f+panY;
    }
    if(reference<=0.f) reference=zoom;
    CanvasView view; view.ready=true; view.scope=scope;
    view.width=width; view.height=height; view.physicalWidth=width; view.physicalHeight=height;
    view.zoom=zoom; view.referenceZoom=reference; view.pan={panX,panY}; view.origin={originX,originY};
    view.grid=grid_; view.trails=trails_;
    return view;
}
void Editor::fitCanvas(ImVec2 start,ImVec2 size,const Json& objects) {
    if(objects.empty()) { pan_={}; zoom_=35; if(referenceZoom_<=0.f) referenceZoom_=zoom_; return; }
    for(int pass=0;pass<4;++pass) {
    ImVec2 low{FLT_MAX,FLT_MAX},high{-FLT_MAX,-FLT_MAX};
    for(const auto& o:objects) {
        const auto b=bounds(o,{o["properties"]["x"],o["properties"]["y"],0,0});
        low.x=std::min(low.x,b.first.x);low.y=std::min(low.y,b.first.y);high.x=std::max(high.x,b.second.x);high.y=std::max(high.y,b.second.y);
    }
    zoom_=std::clamp(std::min((size.x-120)/std::max(.1f,high.x-low.x),(size.y-110)/std::max(.1f,high.y-low.y)),8.f,1200.f);
    pan_={size.x*.25f-(low.x+high.x)*.5f*zoom_,-size.y*.35f+(low.y+high.y)*.5f*zoom_};
    origin_={start.x+size.x*.25f+pan_.x,start.y+size.y*.85f+pan_.y};
    }
    // Only a document without a stored baseline (fresh import) adopts the fit.
    if(referenceZoom_<=0.f) referenceZoom_=zoom_;
}
void Editor::componentSmokeInput(unsigned frame) {
    auto& io=ImGui::GetIO();
    if(frame>=3 && frame<=7) { const auto p=screen(frame<5?0:.2,frame<5?-.3:-.2); io.AddMousePosEvent(p.x,p.y); }
    if(frame==3) io.AddMouseButtonEvent(0,true);
    if(frame==7) io.AddMouseButtonEvent(0,false);
    if(frame>=22 && frame<=26) {
        const float fraction=frame<24?1.f/3:2.f/3;
        io.AddMousePosEvent(historySliderStart_.x+12+(historySliderEnd_.x-historySliderStart_.x-24)*fraction,(historySliderStart_.y+historySliderEnd_.y)/2);
        if(frame==22) io.AddMouseButtonEvent(0,true);
    }
    if(frame==26) io.AddMouseButtonEvent(0,false);
    if(frame==76 || frame==77) { io.AddKeyEvent(ImGuiMod_Ctrl,frame==76); io.AddKeyEvent(ImGuiKey_Z,frame==76); }
}
void Editor::componentSmokeAfterFrame(unsigned frame) {
    auto check=[](bool valid,const char* message){if(!valid) throw std::runtime_error(message);};
    if(frame==10) {
        check(selected_=="apparatus" && std::abs(document().object("apparatus")["properties"]["x"].get<double>()-.2)<.002,"Whole component picking/dragging failed");
        dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","update"},{"id","apparatus"},{"patch",{{"properties",{{"parameters",{{"R",.45}}}}}}}}})}}}},"mcp");
    }
    if(frame==16) { historyOpen_=true; fitRequested_=true; }
    if(frame==23) check(document().previewing() && document().displayRevision()==1,"History slider press did not preview state 1");
    if(frame==27) {
        check(document().previewing() && document().displayRevision()==2 && document().revision()==3,"History slider drag mutated head or missed state 2");
        check(document().history()["lastId"]==3,"History dragging polluted audit log");
    }
    if(frame==29) { dispatch({{"method","preview"}}); historyOpen_=false; fitRequested_=true; }
    if(frame==42) dispatch({{"method","open_component"},{"args",{{"id","incline_arc"}}}},"mcp");
    if(frame==46) {
        dispatch({{"method","apply"},{"scope","component"},{"args",{{"commands",Json::array({{{"op","component_settings"},{"patch",{{"parameters",{{"R",{{"default",.35}}}}}}}}})}}}},"human");
        check(document().revision()==3,"Draft edit changed scene revision");
    }
    if(frame==55) dispatch({{"method","publish_component"},{"expectedRevision",3},{"args",{{"componentRevision",1}}}},"mcp");
    if(frame==75) switchScope("component");
    if(frame==80) {
        check(active().revision()==2 && document().revision()==4,"Human undo crossed document scopes");
        check(active().scene()["componentDefinition"]["parameters"]["R"]["default"]==.3,"Draft Ctrl+Z did not undo draft edit");
    }
    if(frame==86) { historyOpen_=true; fitRequested_=true; }
    if(frame==93) dispatch({{"method","preview"},{"scope","component"},{"args",{{"revision",0}}}},"mcp");
    if(frame==110) {
        check(viewState_["presentedScope"]=="component" && viewState_["presentedRevision"]==0,"Component preview was not presented");
        check(!document().previewing() && document().revision()==4,"Component preview affected scene head");
    }
}
}
