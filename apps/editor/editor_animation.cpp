#include "editor.hpp"
#include "animation.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace physics_editor {
namespace {
bool editNumber(const char* label,double& value) {
    char buffer[64];std::snprintf(buffer,sizeof(buffer),"%.9g",value);
    if(!ImGui::InputText(label,buffer,sizeof(buffer),ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CharsScientific))return false;
    char* end=nullptr;double result=std::strtod(buffer,&end);if(end==buffer || *end || !std::isfinite(result))return false;value=result;return true;
}
}
static float clampSidebarWidth(float value) { return std::clamp(value,140.f,700.f); }
void Editor::storeLayout() {
    workspace_.layout()={{"workspace",animationWorkspace_?"animation":"layout"},{"sidebarWidth",sidebarWidth_},
        {"bottomHeight",bottomHeight_},{"historyOpen",historyOpen_},{"grid",grid_},{"trails",trails_},{"followMcp",followMcp_},
        {"scope",scope_},{"selected",selected_},{"channel",channel_},{"zoom",zoom_},{"pan",{pan_.x,pan_.y}},
        {"referenceZoom",referenceZoom_>0.f?referenceZoom_:zoom_}};
    storeActivityState();
}
void Editor::applyLayout() {
    const auto& l=workspace_.layout();
    animationWorkspace_=l.value("workspace",std::string("layout"))=="animation";
    sidebarWidth_=clampSidebarWidth(l.value("sidebarWidth",320.f));bottomHeight_=std::clamp(l.value("bottomHeight",260.f),180.f,700.f);
    historyOpen_=l.value("historyOpen",false);grid_=l.value("grid",false);trails_=l.value("trails",true);followMcp_=l.value("followMcp",true);
    applyActivityState(l);
    switchScope(l.value("scope",std::string("scene"))=="component" && workspace_.hasComponent()?"component":"scene");
    selected_=l.value("selected",std::string());channel_=l.value("channel",std::string("x"));
    zoom_=std::clamp(l.value("zoom",35.f),8.f,1200.f);if(l.contains("pan")){pan_={l["pan"][0],l["pan"][1]};}
    referenceZoom_=std::clamp(l.value("referenceZoom",zoom_),8.f,1200.f);
}
void Editor::projectDialog() {
    if(projectDialogOpen_){ImGui::OpenPopup("独立工程");projectDialogOpen_=false;}
    if(ImGui::BeginPopupModal("独立工程",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(".physproj 包含场景、组件草稿、动画、独立历史和公式资源。\n打开后保持暂停；可以继续撤销与重做。");
        ImGui::SetNextItemWidth(540);ImGui::InputText("工程路径",projectPath_,sizeof(projectPath_));
        static bool discard=false;ImGui::Checkbox("打开时放弃当前工程的未保存修改",&discard);
        if(ImGui::Button("保存到此路径"))attempt({{"method","save_project"},{"args",{{"path",projectPath_},{"overwrite",true}}}});ImGui::SameLine();
        if(ImGui::Button("打开工程"))attempt({{"method","load_project"},{"args",{{"path",projectPath_},{"discard",discard}}}});ImGui::SameLine();
        if(ImGui::Button("关闭")){discard=false;ImGui::CloseCurrentPopup();}
        ImGui::TextWrapped("%s",status_.c_str());ImGui::EndPopup();
    }
}
void Editor::animationPanel() {
    const Json* found=nullptr;for(const auto& o:active().scene()["objects"])if(o["id"]==selected_)found=&o;
    if(!found){ImGui::TextUnformatted("动画编辑 · 选择一个对象，然后添加属性通道。示例菜单提供公式与阶跃演示。");return;}
    const Json object=*found;const double duration=active().scene()["duration"];
    ImGui::BeginDisabled(active().previewing() || scope_=="component");
    ImGui::BeginChild("channels",{310,0},ImGuiChildFlags_Borders);
    ImGui::Text("动画 · %s",object["name"].get_ref<const std::string&>().c_str());
    if(object.contains("motion") || object.contains("binding")) {
        ImGui::TextWrapped("此对象使用运动模型或绑定。先在属性中移除，再添加显式动画通道。");
        ImGui::EndChild();ImGui::EndDisabled();return;
    }
    Json animation=object.value("animation",Json{{"channels",Json::object()}});
    if(ImGui::BeginCombo("属性通道",channel_.c_str())) {
        for(const char* name:{"x","y","dx","dy","rotation","radius","width","height","scale","vx","vy"}) {
            if(object["properties"].contains(name) || std::string(name)=="vx" || std::string(name)=="vy")if(ImGui::Selectable(name,channel_==name)){channel_=name;selectedKey_=-1;keyDragging_=false;}
        }ImGui::EndCombo();
    }
    auto apply=[&](const Json& a){execute(Json::array({{{"op","set_animation"},{"id",selected_},{"animation",a}}}));};
    if(!animation["channels"].contains(channel_)) {
        const double value=object["properties"].value(channel_,0.0);
        if(ImGui::Button("添加关键帧通道")){animation["channels"][channel_]={{"kind","keys"},{"keys",Json::array({{{"time",0},{"value",value},{"interpolation","linear"}}})}};apply(animation);}
        if(ImGui::Button("添加公式通道")){animation["channels"][channel_]={{"kind","formula"},{"segments",Json::array({{{"start",0},{"end",duration},{"expression",std::to_string(value)}}})}};apply(animation);}
    } else {
        const auto spec=animation["channels"][channel_];
        ImGui::TextDisabled("%s",spec["kind"]=="keys"?"关键帧 · 拖动菱形修改时间和值":"公式 · t 全局时间 / u 分段局部时间");
        if(spec["kind"]=="formula") {
            auto segments=spec["segments"];
            for(size_t i=0;i<segments.size();++i){ImGui::PushID(int(i));
                ImGui::Text("[%.3f, %.3f%s",segments[i]["start"].get<double>(),segments[i]["end"].get<double>(),i+1==segments.size()?"]":")");
                char expression[1025];strcpy_s(expression,segments[i]["expression"].get_ref<const std::string&>().c_str());ImGui::SetNextItemWidth(-1);
                if(ImGui::InputText("##formula",expression,sizeof(expression),ImGuiInputTextFlags_EnterReturnsTrue)){segments[i]["expression"]=expression;animation["channels"][channel_]["segments"]=segments;apply(animation);}
                ImGui::PopID();
            }
            if(ImGui::Button("在当前时刻分段")) {
                const double t=active().time();for(size_t i=0;i<segments.size();++i)if(t>segments[i]["start"].get<double>() && t<segments[i]["end"].get<double>()) {
                    auto next=segments[i];segments[i]["end"]=t;next["start"]=t;segments.insert(segments.begin()+static_cast<Json::difference_type>(i+1),next);animation["channels"][channel_]["segments"]=segments;apply(animation);break;
                }
            }
        } else {
            if(ImGui::Button("在当前时刻插入关键帧"))try {
                const auto value=animation::evaluate(animation,active().time(),duration)["channels"][channel_];
                execute(Json::array({{{"op","set_keyframe"},{"id",selected_},{"channel",channel_},{"time",active().time()},{"value",value},{"interpolation","linear"}}}));
            }catch(const std::exception& e){status_=e.what();}
            if(selectedKey_>=0 && selectedKey_<int(spec["keys"].size())) {
                const auto key=spec["keys"][selectedKey_];const auto mode=key.value("interpolation",std::string("linear"));
                ImGui::Text("关键帧 %d · %.3f s = %.3f",selectedKey_,key["time"].get<double>(),key["value"].get<double>());
                double time=key["time"],value=key["value"];ImGui::SetNextItemWidth(140);
                const bool editedTime=editNumber("时刻 / s",time);ImGui::SetNextItemWidth(140);const bool editedValue=editNumber("属性值",value);
                if(editedTime || editedValue) {
                    bool collision=false;for(const auto& k:spec["keys"])if(k["time"]!=key["time"] && k["time"]==time)collision=true;
                    if(collision)status_="关键帧时间重合，输入未提交。";
                    else execute(Json::array({{{"op","delete_keyframe"},{"id",selected_},{"channel",channel_},{"time",key["time"]}},{{"op","set_keyframe"},{"id",selected_},{"channel",channel_},{"time",time},{"value",value},{"interpolation",mode}}}));
                }
                if(ImGui::BeginCombo("到下一帧",mode.c_str())){for(const char* m:{"linear","step","smooth"})if(ImGui::Selectable(m,mode==m))execute(Json::array({{{"op","set_keyframe"},{"id",selected_},{"channel",channel_},{"time",key["time"]},{"value",key["value"]},{"interpolation",m}}}));ImGui::EndCombo();}
                ImGui::BeginDisabled(spec["keys"].size()==1);if(ImGui::Button("删除关键帧")){execute(Json::array({{{"op","delete_keyframe"},{"id",selected_},{"channel",channel_},{"time",key["time"]}}}));selectedKey_=-1;}ImGui::EndDisabled();
            }
        }
        if(ImGui::SmallButton("移除此通道")){animation["channels"].erase(channel_);apply(animation["channels"].empty()?Json():animation);}
    }
    if(animation.contains("parameters") && !animation["parameters"].empty() && ImGui::CollapsingHeader("公式参数",ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto parameters=animation["parameters"];for(auto it=parameters.begin();it!=parameters.end();++it){double value=it.value();ImGui::SetNextItemWidth(140);if(editNumber(it.key().c_str(),value)){animation["parameters"][it.key()]=value;apply(animation);}}
    }
    if(animation.contains("quantities") && !animation["quantities"].empty() && ImGui::CollapsingHeader("物理量公式",ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto quantities=animation["quantities"];for(auto it=quantities.begin();it!=quantities.end();++it){char expression[1025];strcpy_s(expression,it.value()["expression"].get_ref<const std::string&>().c_str());ImGui::SetNextItemWidth(190);if(ImGui::InputText(it.key().c_str(),expression,sizeof(expression),ImGuiInputTextFlags_EnterReturnsTrue)){animation["quantities"][it.key()]["expression"]=expression;apply(animation);}}
    }
    if(ImGui::CollapsingHeader("参数 / 物理量 / 分段数据")) {
        if(animationObject_!=selected_ || animationRevision_!=active().displayRevision()) {strcpy_s(animationJson_,animation.dump(2).c_str());animationObject_=selected_;animationRevision_=active().displayRevision();}
        ImGui::TextWrapped("parameters 定义常量；quantities 定义 F 等物理量；channels 将物理量映射到图形属性。sin/cos 使用弧度。");
        ImGui::InputTextMultiline("##animation-json",animationJson_,sizeof(animationJson_),{-1,160});
        if(ImGui::Button("应用动画数据"))try{apply(Json::parse(animationJson_));}catch(const std::exception& e){status_=e.what();}
    }
    ImGui::EndChild();ImGui::SameLine();
    ImGui::BeginChild("curve-region",{0,0});
    if(!animation["channels"].contains(channel_)){ImGui::TextUnformatted("添加通道后，在此查看变化曲线。");ImGui::EndChild();ImGui::EndDisabled();return;}
    try {
        ImGui::Text("%s(t) · 点击曲线区域定位时间 · 阶跃在关键时刻取右侧值",channel_.c_str());
        const auto a=ImGui::GetCursorScreenPos();const auto size=ImVec2{std::max(100.f,ImGui::GetContentRegionAvail().x),std::max(100.f,ImGui::GetContentRegionAvail().y-22)};
        curveStart_={a.x+50,a.y+10};curveEnd_={a.x+size.x-15,a.y+size.y-24};
        std::vector<std::pair<double,double>> samples;
        std::vector<double> times;for(int i=0;i<=180;++i)times.push_back(duration*i/180.0);
        const auto spec=animation["channels"][channel_];
        if(spec["kind"]=="keys")for(const auto& k:spec["keys"]){double t=k["time"];if(t>0)times.push_back(std::nextafter(t,0.0));times.push_back(t);}
        else for(const auto& s:spec["segments"]){double t=s["start"];if(t>0)times.push_back(std::nextafter(t,0.0));times.push_back(t);}
        std::sort(times.begin(),times.end());double low=0,high=0;
        for(double t:times){double v=animation::evaluate(animation,t,duration)["channels"][channel_];samples.emplace_back(t,v);low=std::min(low,v);high=std::max(high,v);}
        if(!keyDragging_){double margin=std::max(.5,(high-low)*.15);curveMin_=low-margin;curveMax_=high+margin;}
        auto point=[&](double t,double v){return ImVec2{curveStart_.x+float(t/duration)*(curveEnd_.x-curveStart_.x),curveEnd_.y-float((v-curveMin_)/(curveMax_-curveMin_))*(curveEnd_.y-curveStart_.y)};};
        ImGui::InvisibleButton("##curve",size);const bool hovered=ImGui::IsItemHovered();auto& io=ImGui::GetIO();auto* d=ImGui::GetWindowDrawList();
        d->AddRectFilled(a,{a.x+size.x,a.y+size.y},IM_COL32(30,30,30,255));
        for(int i=0;i<=6;++i){double t=duration*i/6.0;auto p=point(t,curveMin_);d->AddLine({p.x,curveStart_.y},{p.x,curveEnd_.y},IM_COL32(64,64,64,255));char label[40];std::snprintf(label,sizeof(label),"%.2f",t);d->AddText({p.x-12,curveEnd_.y+3},IM_COL32(160,160,160,255),label);}
        for(int i=0;i<=4;++i){double v=curveMin_+(curveMax_-curveMin_)*i/4;auto p=point(0,v);d->AddLine(p,{curveEnd_.x,p.y},IM_COL32(64,64,64,255));char label[40];std::snprintf(label,sizeof(label),"%.2g",v);d->AddText({a.x,p.y-6},IM_COL32(160,160,160,255),label);}
        for(size_t i=1;i<samples.size();++i)d->AddLine(point(samples[i-1].first,samples[i-1].second),point(samples[i].first,samples[i].second),IM_COL32(239,163,70,255),2);
        const auto cursor=point(active().time(),0);d->AddLine({cursor.x,curveStart_.y},{cursor.x,curveEnd_.y},IM_COL32(90,170,230,255),2);
        int hit=-1;
        if(spec["kind"]=="keys")for(size_t i=0;i<spec["keys"].size();++i){const auto k=spec["keys"][i];auto p=point(keyDragging_ && selectedKey_==int(i)?keyTime_:k["time"].get<double>(),keyDragging_ && selectedKey_==int(i)?keyValue_:k["value"].get<double>());
            ImVec2 vertices[]={{p.x,p.y-6},{p.x+6,p.y},{p.x,p.y+6},{p.x-6,p.y}};d->AddConvexPolyFilled(vertices,4,selectedKey_==int(i)?IM_COL32(255,220,150,255):IM_COL32(220,140,45,255));if(std::hypot(io.MousePos.x-p.x,io.MousePos.y-p.y)<12)hit=int(i);
        }
        if(hovered && ImGui::IsMouseClicked(0) && hit>=0){selectedKey_=hit;keyDragging_=true;keyMoved_=false;keyRevision_=active().revision();keyOldTime_=keyTime_=spec["keys"][hit]["time"];keyValue_=spec["keys"][hit]["value"];active().dispatch({{"method","playback"},{"args",{{"playing",false}}}},"preview");}
        if(keyDragging_) {
            if(active().revision()!=keyRevision_){keyDragging_=false;status_="关键帧拖动已取消：文档已更新。";}
            else {
                keyMoved_=keyMoved_ || ImGui::IsMouseDragging(0);
                if(keyMoved_){keyTime_=std::clamp(double((io.MousePos.x-curveStart_.x)/(curveEnd_.x-curveStart_.x))*duration,0.0,duration);
                keyValue_=std::clamp(curveMin_+double((curveEnd_.y-io.MousePos.y)/(curveEnd_.y-curveStart_.y))*(curveMax_-curveMin_),-1e6,1e6);}
                if(ImGui::IsMouseReleased(0)) {
                    keyDragging_=false;bool duplicate=false;for(const auto& k:spec["keys"])if(k["time"]!=keyOldTime_ && std::abs(k["time"].get<double>()-keyTime_)<1e-6)duplicate=true;
                    if(duplicate)status_="关键帧时间重合，拖动已取消。";
                    else if(keyMoved_ && (std::abs(keyOldTime_-keyTime_)>1e-6 || std::abs(spec["keys"][selectedKey_]["value"].get<double>()-keyValue_)>1e-6)) {
                        execute(Json::array({{{"op","delete_keyframe"},{"id",selected_},{"channel",channel_},{"time",keyOldTime_}},{{"op","set_keyframe"},{"id",selected_},{"channel",channel_},{"time",keyTime_},{"value",keyValue_},{"interpolation",spec["keys"][selectedKey_].value("interpolation",std::string("linear"))}}}));selectedKey_=-1;
                    }
                }
            }
        }else if(hovered && hit<0 && ImGui::IsMouseDown(0)) {
            const double t=std::clamp(double((io.MousePos.x-curveStart_.x)/(curveEnd_.x-curveStart_.x))*duration,0.0,duration);
            active().dispatch({{"method","playback"},{"args",{{"time",t},{"playing",false}}}},"preview");
            timeSeeking_=true;
        }
        if(timeSeeking_ && ImGui::IsMouseReleased(0)){timeSeeking_=false;attempt({{"method","playback"},{"args",{{"time",active().time()},{"playing",false}}}});}
    }catch(const std::exception& e){ImGui::TextWrapped("曲线求值失败：%s",e.what());}
    ImGui::EndChild();ImGui::EndDisabled();
}
void Editor::animationSmokeInput(unsigned frame) {
    auto& io=ImGui::GetIO();
    if(frame==1){dispatch({{"method","replace"},{"args",{{"document",animation::demo()}}}},"system");animationWorkspace_=true;selected_="block";channel_="y";fitRequested_=true;}
    if(frame>=10 && frame<=12) {
        const float x=curveStart_.x+(curveEnd_.x-curveStart_.x)*.5f;
        const float y=curveEnd_.y-float((1.5-curveMin_)/(curveMax_-curveMin_))*(curveEnd_.y-curveStart_.y);
        io.AddMousePosEvent(x+(frame>10?24:0),y-(frame>10?10:0));
        if(frame==10)io.AddMouseButtonEvent(0,true);if(frame==12)io.AddMouseButtonEvent(0,false);
    }
    if(frame==20){io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_Z,true);}
    if(frame==21){io.AddKeyEvent(ImGuiKey_Z,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);}
    if(frame==40){selected_="force";channel_="dy";dispatch({{"method","playback"},{"args",{{"time",.5},{"playing",false}}}},"mcp");}
    if(frame==90){selected_="block";channel_="x";auto a=active().object("block")["animation"];a["channels"]["x"]["segments"]=Json::array({{{"start",0},{"end",6},{"expression","1/(t-0.123456)"}}});execute(Json::array({{{"op","set_animation"},{"id","block"},{"animation",a}}}));}
    if(frame==91)dispatch({{"method","playback"},{"args",{{"time",.123456},{"playing",false}}}},"mcp");
}
void Editor::animationSmokeAfterFrame(unsigned frame) {
    auto check=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    if(frame==14){const auto keys=active().object("block")["animation"]["channels"]["y"]["keys"];check(active().revision()==2 && keys[1]["time"].get<double>()>3 && keys[1]["value"].get<double>()>1.5,"GUI keyframe drag did not commit one transaction");check(active().history()["entries"].back()["source"]=="human","GUI key source missing");}
    if(frame==23)check(active().object("block")["animation"]==animation::demo()["objects"][1]["animation"],"Human undo did not restore keyframe");
    if(frame==45){const auto value=animation::properties(active().object("force"),.5,6)["dy"].get<double>();check(std::abs(value-2*std::sin(1.0))<1e-9,"Nonlinear arrow rendering sample differs");check(drawnTime_==.5,"MCP seek did not reach canvas");}
    if(frame==94)check(!active().playing() && status_.find("动画求值失败")!=std::string::npos,"Runtime expression failure not displayed safely");
    if(frame==115)std::printf("PASS: animation workspace, real key drag, one human transaction, Ctrl+Z, nonlinear arrow, MCP seek, runtime domain error containment\n");
}
}
