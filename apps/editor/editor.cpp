#include "editor.hpp"
#include "editor_write.hpp"
#include "formula.hpp"
#include "component.hpp"
#include "animation.hpp"
#include "write_entry.hpp"
#include "vulkan_host.hpp"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
// Strict UTF-8 decoder used for glyph coverage. It never truncates a code point
// to ImWchar16: code points above the atlas limit are reported as missing with
// their full value instead of silently colliding with a different BMP glyph.
// Invalid bytes decode to U+FFFD so malformed authoring text is still surfaced.
std::vector<uint32_t> decodeUtf8(const std::string& text) {
    std::vector<uint32_t> result; result.reserve(text.size());
    const auto* bytes=reinterpret_cast<const unsigned char*>(text.data());
    for(size_t i=0;i<text.size();) {
        const unsigned char lead=bytes[i]; uint32_t cp=0; size_t length=0;
        if(lead<0x80) { cp=lead; length=1; }
        else if((lead&0xE0)==0xC0) { cp=lead&0x1Fu; length=2; }
        else if((lead&0xF0)==0xE0) { cp=lead&0x0Fu; length=3; }
        else if((lead&0xF8)==0xF0) { cp=lead&0x07u; length=4; }
        else { result.push_back(0xFFFDu); ++i; continue; }
        if(i+length>text.size()) { result.push_back(0xFFFDu); ++i; continue; }
        bool valid=true;
        for(size_t k=1;k<length;++k) {
            if((bytes[i+k]&0xC0)!=0x80) { valid=false; break; }
            cp=(cp<<6)|(bytes[i+k]&0x3Fu);
        }
        if(!valid) { result.push_back(0xFFFDu); ++i; continue; }
        const bool overlong=(length==2 && cp<0x80)||(length==3 && cp<0x800)||(length==4 && cp<0x10000);
        const bool surrogate=(cp>=0xD800 && cp<=0xDFFF);
        if(overlong || surrogate || cp>0x10FFFF) result.push_back(0xFFFDu);
        else result.push_back(cp);
        i+=length;
    }
    return result;
}
std::string encodeUtf8(uint32_t cp) {
    std::string out;
    if(cp<0x80) out.push_back(char(cp));
    else if(cp<0x800) { out.push_back(char(0xC0|(cp>>6))); out.push_back(char(0x80|(cp&0x3F))); }
    else if(cp<0x10000) { out.push_back(char(0xE0|(cp>>12))); out.push_back(char(0x80|((cp>>6)&0x3F))); out.push_back(char(0x80|(cp&0x3F))); }
    else { out.push_back(char(0xF0|(cp>>18))); out.push_back(char(0x80|((cp>>12)&0x3F))); out.push_back(char(0x80|((cp>>6)&0x3F))); out.push_back(char(0x80|(cp&0x3F))); }
    return out;
}
// 操作日志的展示辅助：查询/普通/失败三态着色，与底部撤销历史刻意区分。
const char* activityStatusText(const ActivityItem& item) {
    if(item.status=="error") return "失败";
    if(item.status=="warning") return "警告";
    return item.query?"查询":"成功";
}
ImVec4 activityStatusColor(const ActivityItem& item) {
    if(item.status=="error") return {1.f,.45f,.35f,1};
    if(item.status=="warning") return {1.f,.72f,.28f,1};
    return item.query?ImVec4{.62f,.66f,.70f,1}:ImVec4{.55f,.82f,.55f,1};
}
// 人类/AI 用同一套元操作名，界面只加中文标签，日志里始终保留方法名本身。
const char* activitySourceLabel(const std::string& source) {
    return source=="mcp"?"MCP":source=="human"?"界面":source=="system"?"系统":source=="job"?"任务":"?";
}
const char* activityScopeLabel(const std::string& scope) {
    return scope=="scene"?"主场景":scope=="component"?"组件":scope=="-"?"-":"?";
}
// close_component 永远回到主场景，不能沿用请求里的 scope。
std::string activityResolvedScope(const std::string& method,const std::string& scope) {
    if(method=="close_component") return "scene";
    return scope;
}
// 左栏拖动范围与 storeLayout/applyLayout 使用同一组上下限，避免只有拖动路径收窄。
float activityClampSidebar(float value) { return std::clamp(value,140.f,700.f); }
}
Editor::Editor() {
    initializeRenderers();
    Json masks=Json::array(); for(const auto& e:formula_cache::entries) masks.push_back({{"name",e.name},{"width",e.width},{"height",e.height},{"rle",std::vector<unsigned char>(e.rle,e.rle+e.bytes)}});
    workspace_.setResources({{"formulaMasks",masks},{"formulaScale",formula_cache::displayScale}});
    workspace_.validateFormulas();referenceZoom_=zoom_;
    activityLastId_=activityLog_.lastId();
}
// ---------------------------------------------------------------------------
// 操作日志：写入。UI 与 MCP 都经由 dispatch 进入，因此两边记录完全同构。
// ---------------------------------------------------------------------------
void Editor::recordActivityLine(const Json& request,const std::string& source,bool ok,
                                const std::string& error,const std::string& scope) {
    const std::string method=request.value("method",std::string("invalid"));
    // 轮询类只读请求成功时静默：否则 get_scene/get_job 轮询会瞬间刷满有界日志。
    // 它们一旦失败仍然可见（isQueryMethod 只影响成功态）。
    if(ok && ActivityLog::isQueryMethod(method)) return;
    // get_activity_log 永远不记录自己：否则「查看日志」本身就会改变日志。
    if(method=="get_activity_log") return;
    const std::string resolved=scope.empty()?request.value("scope",std::string("-")):scope;
    // journalWarning 属于「操作成功但会话日志写盘失败」：保留为独立的 warning 状态，
    // 这样「失败」筛选不会把它和真正的拒绝混为一谈，也不会被静默吞掉。
    const std::string state=(ok && !error.empty())?"warning":(ok?"ok":"error");
    const std::string summary=ActivityLog::describeRequest(request);
    activityLog_.recordRequestDetailed(request,source,state,error,summary,resolved);
}
uint64_t Editor::recordActivity(const std::string& method,const std::string& scope,const std::string& source,
                                bool ok,const std::string& summary,const std::string& error,const Json& detail) {
    return activityLog_.record(method,scope,source,ok,summary,error,detail);
}
uint64_t Editor::recordActivityJob(const JobOutcome& outcome) {
    return activityLog_.recordJob(outcome.kind,outcome.jobId,outcome.scope,outcome.source,
        outcome.status,outcome.error,outcome.summary,outcome.startedId);
}
void Editor::storeActivityState() {
    auto& layout=workspace_.layout();
    layout["activityOpen"]=activityOpen_;layout["activityWidth"]=activityWidth_;
    layout["activityFollow"]=activityFollow_;layout["activityShowUi"]=activityShowUi_;
    layout["activityShowMcp"]=activityShowMcp_;layout["activityShowFailed"]=activityShowFailed_;
    layout["activityShowQuiet"]=activityShowQuiet_;
}
void Editor::applyActivityState(const Json& layout) {
    activityOpen_=layout.value("activityOpen",true);
    activityWidth_=std::clamp(layout.value("activityWidth",340.f),240.f,700.f);
    activityFollow_=layout.value("activityFollow",true);
    activityShowUi_=layout.value("activityShowUi",true);
    activityShowMcp_=layout.value("activityShowMcp",true);
    activityShowFailed_=layout.value("activityShowFailed",true);
    activityShowQuiet_=layout.value("activityShowQuiet",false);
}
// 冒烟测试的即时快照（不经过 dispatch，因此查询本身不会入日志）。
Json Editor::activitySnapshot() const {
    Json methods=Json::array();
    for(const auto& item:activityLog_.items()) methods.push_back(item.method);
    Json result=activityLog_.query(0,ActivityLog::kCapacity);
    result["sidebar"]={{"open",activityOpen_},{"visible",activityVisible_},{"collapsedBar",activityCollapsedBar_},
        {"following",activityFollowing_},{"follow",activityFollow_},{"width",activityWidth_}};
    // 后台任务进度面板本帧画在哪里（日志栏内 / 兜底浮层 / 没画），供冒烟与验证查询。
    result["jobs"]=jobSnapshot();
    result["methods"]=methods;
    return result;
}

ImVec2 Editor::screen(double x,double y) const { return {origin_.x+static_cast<float>(x)*zoom_,origin_.y-static_cast<float>(y)*zoom_}; }
ImVec2 Editor::world(ImVec2 p) const { return {(p.x-origin_.x)/zoom_,(origin_.y-p.y)/zoom_}; }
// Pixel-sized primitives (text "size", formula "scale") are multiplied by this
// factor when drawn, so a view zoom scales the complete drawing instead of only
// the world-space geometry. At the reference zoom the factor is exactly 1 and
// documents render as authored.
float Editor::canvasScale() const {
    if(referenceZoom_<=0.f) return 1.f;
    return zoom_/referenceZoom_;
}
void Editor::resetPixelReference(float zoom) {
    referenceZoom_=zoom>0.f?zoom:35.f;
    status_="像素基准已重设为 "+std::to_string(static_cast<int>(std::lround(referenceZoom_)))+" px/m：文字与公式在此缩放下为标称大小。";
}
// Line widths and decorations are authored in screen pixels too. Scaling them
// keeps the drawing self-similar; the floor only guards against hairlines that
// could not be rasterized at all.
float Editor::strokeWidth(const Json& p) const { return std::max(0.75f,f(p,"stroke")*canvasScale()); }
void Editor::arrow(ImDrawList* list,ImVec2 a,ImVec2 b,ImU32 tint,float stroke) {
    const float length=distance(a,b); if(length<0.5f) return;
    const float ux=(b.x-a.x)/length, uy=(b.y-a.y)/length,head=std::min(12.0f*canvasScale(),length*0.4f);
    list->AddLine(a,b,tint,stroke);
    list->AddTriangleFilled(b,{b.x-head*ux+head*.4f*uy,b.y-head*uy-head*.4f*ux},
        {b.x-head*ux-head*.4f*uy,b.y-head*uy+head*.4f*ux},tint);
}
void Editor::initializeRenderers() {
    renderers_["point"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { d->AddCircleFilled(screen(s.x,s.y),std::max(3.f*canvasScale(),f(o["properties"],"radius")*zoom_),c); };
    renderers_["circle"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; d->AddCircle(screen(s.x,s.y),f(p,"radius")*zoom_,c,64,strokeWidth(p)); };
    renderers_["rectangle"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; const double a=p.value("rotation",0.0)*3.141592653589793/180,co=std::cos(a),si=std::sin(a);
        const double w=p["width"],h=p["height"]; ImVec2 points[4]; int i=0;
        for(const auto q:{ImVec2{0,0},ImVec2{float(w),0},ImVec2{float(w),float(h)},ImVec2{0,float(h)}})
            points[i++]=screen(s.x+co*q.x-si*q.y,s.y+si*q.x+co*q.y);
        d->AddPolyline(points,4,c,ImDrawFlags_Closed,strokeWidth(p));
    };
    renderers_["arc"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; ImVec2 points[129]; const double start=p["start"],end=p["end"],r=p["radius"];
        for(int i=0;i<=128;++i) { const double a=(start+(end-start)*i/128)*3.141592653589793/180; points[i]=screen(s.x+r*std::cos(a),s.y+r*std::sin(a)); }
        d->AddPolyline(points,129,c,0,strokeWidth(p));
    };
    renderers_["component"]=[this](ImDrawList* d,const Json& o,State s,ImU32) {
        for(const auto& child:components::instantiate(active().scene(),o,s)) {
            if(!child["visible"].get<bool>() || child["step"].get<int>()>active().step()) continue;
            const auto renderer=renderers_.find(child["type"].get<std::string>());
            if(renderer!=renderers_.end()) renderer->second(d,child,{child["properties"]["x"],child["properties"]["y"],0,0},color(child["properties"]));
        }
    };
    renderers_["line"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; d->AddLine(screen(s.x,s.y),screen(s.x+f(p,"dx"),s.y+f(p,"dy")),c,strokeWidth(p)); };
    renderers_["arrow"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; double dx=f(p,"dx"),dy=f(p,"dy");
        if(o.contains("binding") && o["binding"]["kind"]=="velocity") { const double scale=o["binding"]["scale"]; dx=s.vx*scale; dy=s.vy*scale; }
        arrow(d,screen(s.x,s.y),screen(s.x+dx,s.y+dy),c,strokeWidth(p));
    };
    renderers_["ground"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; const auto a=screen(s.x,s.y), b=screen(s.x+f(p,"dx"),s.y+f(p,"dy"));
        const float scale=canvasScale();
        d->AddLine(a,b,c,strokeWidth(p)); const float length=distance(a,b); if(length<1) return;
        const float ux=(b.x-a.x)/length,uy=(b.y-a.y)/length;
        for(float t=15*scale;t<length && t<5000;t+=24*scale) { ImVec2 q{a.x+ux*t,a.y+uy*t}; d->AddLine(q,{q.x-8*scale*ux-12*scale*uy,q.y-8*scale*uy+12*scale*ux},c,std::max(0.75f,1.4f*scale)); }
    };
    renderers_["text"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; const float size=f(p,"size")*canvasScale(); const auto pos=screen(s.x,s.y);
        const char* text=p["text"].get_ref<const std::string&>().c_str();
        try { if(write_draw::text(d,o,pos,size,text,c,active().time(),canvasScale())) return; }
        catch(const std::exception&) {}
        d->AddText(ImGui::GetFont(),size,pos,c,text);
    };
    renderers_["formula"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) {
        const auto& p=o["properties"]; const auto pos=screen(s.x,s.y); const float scale=f(p,"scale")*canvasScale();
        try {
            if(write_draw::formula(d,o,pos,c,scale,active().time(),canvasScale(),workspace_.resourceGeneration())) return;
            math_text::draw(d,pos,p["formula"].get_ref<const std::string&>().c_str(),c,scale);
        }
        catch(const std::exception&) { d->AddText(pos,IM_COL32(200,30,30,255),"未知公式资源"); }
    };
    renderers_["polyline"]=[this](ImDrawList* d,const Json& o,State s,ImU32 c) { const auto& p=o["properties"]; const auto& points=p["points"];
        for(size_t i=1;i<points.size();++i) d->AddLine(screen(s.x+points[i-1][0].get<double>(),s.y+points[i-1][1].get<double>()),screen(s.x+points[i][0].get<double>(),s.y+points[i][1].get<double>()),c,strokeWidth(p));
    };
}
void Editor::windowState(bool visible,bool minimized,unsigned frames) {
    viewState_["visible"]=visible; viewState_["minimized"]=minimized; viewState_["presentedFrames"]=frames;
}
void Editor::publishPresentedView() {
    viewState_["presentedRevision"]=drawnRevision_; viewState_["presentedTime"]=drawnTime_; viewState_["presentedScope"]=drawnScope_;
    if(!lastFrameViewValid_) return;
    const Json presented=frameViewJson(lastFrameView_,"presented");
    viewState_["presented"]=presented;
    for(const char* key:{"canvasReady","canvasWidth","canvasHeight","zoom","referenceZoom","origin","pan","scope","framebufferScale"})
        viewState_[key]=presented[key];
    viewState_["viewportSource"]="presented";
}
void Editor::presented() { publishPresentedView(); }
Json Editor::frameViewJson(const CanvasView& view,const char* source) const {
    Json result={{"source",source},{"canvasReady",false},{"scope",view.scope.empty()?Json():Json(view.scope)},
        {"canvasWidth",nullptr},{"canvasHeight",nullptr},{"zoom",nullptr},{"referenceZoom",nullptr},
        {"origin",nullptr},{"pan",nullptr},{"framebufferScale",{view.framebufferScaleX,view.framebufferScaleY}}};
    if(!view.ready || view.width<=0 || view.height<=0 || view.zoom<=0) return result;
    // Reported viewport geometry is in physical framebuffer pixels; canvas-local
    // origin keeps +y pointing up in world space.
    result["canvasReady"]=true;
    result["canvasWidth"]=view.physicalWidth;
    result["canvasHeight"]=view.physicalHeight;
    result["zoom"]=view.zoom*view.framebufferScaleX;
    result["referenceZoom"]=view.referenceZoom*view.framebufferScaleX;
    result["origin"]={view.origin.x*view.framebufferScaleX,view.origin.y*view.framebufferScaleY};
    result["pan"]={view.pan.x*view.framebufferScaleX,view.pan.y*view.framebufferScaleY};
    return result;
}
void Editor::publishFrameView() {
    const auto scale=ImGui::GetIO().DisplayFramebufferScale;
    CanvasView view; view.scope=scope_;
    view.framebufferScaleX=scale.x>0?scale.x:1.f; view.framebufferScaleY=scale.y>0?scale.y:1.f;
    view.width=lastCanvasSize_.x; view.height=lastCanvasSize_.y;
    view.zoom=zoom_; view.referenceZoom=referenceZoom_>0?referenceZoom_:zoom_;
    view.origin={origin_.x-lastCanvasStart_.x,origin_.y-lastCanvasStart_.y};
    view.pan=pan_; view.grid=grid_; view.trails=trails_;
    // A degenerate canvas is reported as not ready instead of fabricating a
    // viewport; the last usable recorded composition stays cached for export.
    if(view.width<32.f || view.height<32.f || view.zoom<=0.f) {
        lastFrameView_=view; lastFrameViewValid_=true;
        viewState_["view"]=frameViewJson(view,"live");
        return;
    }
    view.ready=true;
    view.physicalWidth=view.width*view.framebufferScaleX; view.physicalHeight=view.height*view.framebufferScaleY;
    recordedViews_[scope_]=view; lastFrameView_=view; lastFrameViewValid_=true;
    viewState_["view"]=frameViewJson(view,"live");
}
void Editor::invalidateRecordedViews(const std::string& scope) {
    if(scope.empty()) recordedViews_.clear();
    else recordedViews_.erase(scope);
}
Editor::CanvasView Editor::recordedView(const std::string& scope) const {
    const auto found=recordedViews_.find(scope);
    return found==recordedViews_.end()?CanvasView{}:found->second;
}
void Editor::collectTextSamples(const Document& doc,bool componentScope,std::vector<TextSample>& out) const {
    const auto& scene=doc.scene();
    auto add=[&](const std::string& id,const Json& object) {
        if(object.at("type")!="text" || !object.at("properties").contains("text")) return;
        out.push_back({id,object.at("properties").at("text").get<std::string>()});
    };
    if(componentScope) {
        // A failed materialization is a real authoring/asset error: the caller
        // decides how to surface it. Swallowing it here would let the export
        // bake an atlas without the text that is actually drawn.
        for(const auto& child:components::materialize(scene)) add(child.at("id").get<std::string>(),child);
        return;
    }
    for(const auto& object:scene.at("objects")) {
        if(object.at("type")=="component") {
            const auto& p=object.at("properties");
            for(const auto& child:components::instantiate(scene,object,{p.at("x"),p.at("y"),0,0}))
                add(object.at("id").get<std::string>()+"/"+child.at("id").get<std::string>(),child);
        } else add(object.at("id").get<std::string>(),object);
    }
}
void Editor::refreshTextWarnings() {
    const uint64_t generation=scope_=="component"?componentGeneration_:workspaceGeneration_;
    const auto fontAddress=reinterpret_cast<uintptr_t>(ImGui::GetCurrentContext()?ImGui::GetFont():nullptr);
    const std::string key=scope_+":"+std::to_string(generation)+":"+std::to_string(active().displayRevision())+":"+std::to_string(fontAddress);
    if(textWarningValid_ && textWarningKey_==key) return;
    if(fontAddress==0) return; // keep the last known list until a frame has a font
    ImFont* font=ImGui::GetFont();
    std::vector<TextSample> samples;
    try { collectTextSamples(active(),scope_=="component",samples); }
    catch(const std::exception&) {
        // A scene that cannot be materialized has no reliable warning list.
        // Drop the scope cache instead of leaving warnings that belong to a
        // document that may already have been replaced.
        textWarnings_=Json::array(); textWarningKey_.clear(); textWarningValid_=false;
        viewState_["textWarnings"]=textWarnings_; viewState_["textWarningCount"]=0;
        return;
    }
    Json warnings=Json::array(); std::set<std::string> seen;
    for(const auto& sample:samples) {
        for(const uint32_t cp:decodeUtf8(sample.text)) {
            if(cp<0x20 || cp==0x7F) continue;
            const bool missing=cp>0xFFFF || font->FindGlyphNoFallback(static_cast<ImWchar>(cp))==nullptr;
            if(!missing) continue;
            if(!seen.insert(sample.object+":"+std::to_string(cp)).second) continue;
            if(warnings.size()>=64) break;
            warnings.push_back({{"object",sample.object},{"codepoint",cp},{"character",encodeUtf8(cp)},
                {"text",sample.text.size()>120?sample.text.substr(0,120):sample.text}});
        }
        if(warnings.size()>=64) break;
    }
    textWarnings_=std::move(warnings); textWarningKey_=key; textWarningValid_=true;
    viewState_["textWarnings"]=textWarnings_;
    viewState_["textWarningCount"]=textWarnings_.size();
}
void Editor::beginColorEdit() {
    colorEditing_=true; colorEditTarget_=scope_+":"+selected_; colorEditRevision_=active().revision();
    try {
        const auto& c=active().object(selected_).at("properties").at("color");
        for(int i=0;i<4;++i) colorPreview_[i]=c[i];
    } catch(const std::exception&) { colorPreview_[0]=colorPreview_[1]=colorPreview_[2]=colorPreview_[3]=1.f; }
}
void Editor::cancelColorEdit(const std::string& reason) {
    if(!colorEditing_) return;
    colorEditing_=false; colorEditTarget_.clear();
    if(!reason.empty()) status_=reason;
}
void Editor::commitColorEdit() {
    if(!colorEditing_) return;
    const std::string target=colorEditTarget_; const uint64_t revision=colorEditRevision_;
    colorEditing_=false; colorEditTarget_.clear();
    if(target!=scope_+":"+selected_) { status_="颜色编辑已取消：选择或范围已切换。"; return; }
    if(active().revision()!=revision) { status_="颜色编辑已取消：文档已被其他操作更新，未覆盖对方修改。"; return; }
    execute(Json::array({{{"op","update"},{"id",selected_},
        {"patch",{{"properties",{{"color",{colorPreview_[0],colorPreview_[1],colorPreview_[2],colorPreview_[3]}}}}}}}}));
}
void Editor::beginStepEdit() {
    stepEditing_=true; stepEditTarget_=scope_+":"+selected_; stepEditRevision_=active().revision();
    try { stepPreview_=active().object(selected_).at("step").get<int>(); } catch(const std::exception&) { stepPreview_=0; }
}
void Editor::cancelStepEdit(const std::string& reason) {
    if(!stepEditing_) return;
    stepEditing_=false; stepEditTarget_.clear();
    if(!reason.empty()) status_=reason;
}
void Editor::commitStepEdit() {
    if(!stepEditing_) return;
    const std::string target=stepEditTarget_; const uint64_t revision=stepEditRevision_;
    stepEditing_=false; stepEditTarget_.clear();
    if(target!=scope_+":"+selected_) { status_="出现步骤编辑已取消：选择或范围已切换。"; return; }
    if(active().revision()!=revision) { status_="出现步骤编辑已取消：文档已被其他操作更新，未覆盖对方修改。"; return; }
    execute(Json::array({{{"op","update"},{"id",selected_},{"patch",{{"step",stepPreview_}}}}}));
}
float Editor::exportPixelScale(float targetWidth,float targetHeight) const {
    float zoom=zoom_,reference=referenceZoom_>0?referenceZoom_:zoom_;
    if(exportSnapshot_.is_object() && exportSnapshot_.contains("zoom")) {
        const float canvasWidth=exportSnapshot_.value("canvasWidth",0.f),canvasHeight=exportSnapshot_.value("canvasHeight",0.f);
        const float scale=(canvasWidth>0&&canvasHeight>0&&targetWidth>0&&targetHeight>0)
            ?std::min(targetWidth/canvasWidth,targetHeight/canvasHeight):1.f;
        zoom=exportSnapshot_.value("zoom",zoom_)*scale;
        reference=exportSnapshot_.value("referenceZoom",reference);
    }
    if(!(reference>0.f) || !(zoom>0.f)) return 1.f;
    return zoom/reference;
}
void Editor::configureExportFonts(graphics::Options& options) {
    const Document& doc=active();
    const bool componentScope=scope_=="component";
    const auto& scene=doc.scene();
    const double duration=scene.value("duration",3.0);
    // Frame i is rendered at i/fps by exportFrame(frame,fps). That fps travels
    // with the export snapshot (written by jobRequest), so these samples are the
    // exact exported instants: never an approximation such as duration/(frames-1).
    // A call without a frozen export session is a programming error, not a case
    // to paper over with a guessed time base.
    if(options.exportFrames<1 || options.exportFrames>3600)
        throw std::runtime_error("configureExportFonts requires an active export session with 1..3600 frames");
    const unsigned frames=options.exportFrames;
    // Never call value() on a null snapshot (json type_error.306); treat a
    // missing/foreign snapshot as "no session" and fail with our own message.
    const double fps=exportSnapshot_.is_object()?exportSnapshot_.value("fps",0.0):0.0;
    if(!(fps>0.0))
        throw std::runtime_error("Export snapshot carries no fps; the glyph raster size cannot be aligned with exportFrame(frame,fps)");
    const float scale=exportPixelScale(static_cast<float>(options.exportWidth),static_cast<float>(options.exportHeight));

    // Actual glyph sources: every text object plus the text materialized from a
    // component instance, including its parameter template ("R = {R} m"). Text
    // content cannot be animated (channels target numeric properties only), so
    // one pass covers every frame; only the raster size varies per frame. A
    // failure here is surfaced instead of silently baking an incomplete atlas.
    std::vector<TextSample> samples;
    collectTextSamples(doc,componentScope,samples);
    float maxSize=0.f;
    auto addSize=[&](const Json& properties) {
        const float size=properties.value("size",18.f);
        if(std::isfinite(size)) maxSize=std::max(maxSize,size*scale);
    };
    if(componentScope) {
        // Definition children are static (validateDefinition rejects animation),
        // so their text size is fixed; the text itself was collected above.
        for(const auto& child:components::materialize(scene)) addSize(child.at("properties"));
    } else {
        std::vector<Json> animatedText;
        for(const auto& object:scene.at("objects")) {
            if(object.at("type")=="component") {
                const auto& p=object.at("properties");
                for(const auto& child:components::instantiate(scene,object,{p.at("x"),p.at("y"),0,0})) addSize(child.at("properties"));
            } else if(object.at("type")=="text" && object.contains("animation")) {
                animatedText.push_back(object);
            } else addSize(object.at("properties"));
        }
        // Animation replaces the authored size, so evaluate every exported frame
        // at i/fps and take the true maximum. A failed evaluation must abort the
        // export with a clear error, never silently bake too small an atlas.
        for(unsigned frame=0;frame<frames;++frame) {
            const double time=std::min(duration,static_cast<double>(frame)/fps);
            for(const auto& object:animatedText) addSize(animation::properties(object,time,duration));
        }
    }

    // The host bakes the characters in one UTF-8 string; concatenating valid
    // UTF-8 strings stays valid, so no separator is inserted.
    std::set<std::string> unique;
    std::string glyphs;
    for(const auto& sample:samples) if(unique.insert(sample.text).second) glyphs+=sample.text;
    if(!glyphs.empty()) options.exportGlyphText=glyphs;
    if(maxSize>0.f) options.exportFontPixelSize=maxSize;
}
Json Editor::dispatch(const Json& request,const std::string& source) {
    const std::string method=request.at("method");
    // 只读查询：返回最近的操作日志，以及侧栏当前的可见/跟随状态。
    // 人类与 MCP 走同一入口，MCP 用 after 游标 bounded 拉取即可。
    if(method=="get_activity_log") {
        const auto args=request.value("args",Json::object());
        uint64_t after=0; size_t limit=100;
        if(args.contains("after")) {
            if(!args["after"].is_number_unsigned() && !(args["after"].is_number_integer() && args["after"].get<long long>()>=0))
                throw std::runtime_error("after must be a non-negative integer");
            after=args["after"].get<uint64_t>();
        }
        if(args.contains("limit")) {
            if(!args["limit"].is_number_integer() || args["limit"].get<long long>()<1 || args["limit"].get<long long>()>500)
                throw std::runtime_error("limit must be an integer from 1 to 500");
            limit=args["limit"].get<size_t>();
        }
        Json result=activityLog_.query(after,limit);
        // 查询与界面使用同一套筛选语义：MCP 看到的「可见记录」就是用户在侧栏里
        // 看到的那一批（默认静默的只读查询同样不会出现在结果里，失败除外）。
        Json visible=Json::array();
        uint64_t windowCursor=result.value("nextAfter",after);
        uint64_t filteredCursor=0;
        for(const auto& entry:result["entries"]) {
            const std::string itemSource=entry.value("source",std::string());
            const std::string itemStatus=entry.value("status",std::string());
            const bool showSource=itemSource=="mcp"||itemSource=="job"?activityShowMcp_:activityShowUi_;
            const bool failed=itemStatus=="error"||itemStatus=="warning";
            if(!showSource || (failed && !activityShowFailed_)) continue;
            if(entry.value("query",false) && !failed && !activityShowQuiet_) continue;
            visible.push_back(entry);
            filteredCursor=entry.value("id",uint64_t(0));
        }
        result["entries"]=visible;
        result["count"]=visible.size();
        result["nextAfter"]=visible.empty()?windowCursor:filteredCursor;
        result["sidebar"]={{"open",activityOpen_},{"visible",activityVisible_},{"collapsedBar",activityCollapsedBar_},
            {"following",activityFollowing_},{"follow",activityFollow_},{"width",activityWidth_},
            {"filters",{{"ui",activityShowUi_},{"mcp",activityShowMcp_},{"failed",activityShowFailed_},{"quiet",activityShowQuiet_}}},
            // 底部「编辑历史」是另一个面板；两者独立，不能互相替代。
            {"independentOfHistory",true}};
        // 日志栏收起时后台任务进度改画在固定浮层里：这个字段让验证能断言「进度与
        // 取消按钮真的在可见面板上」，而不是只看有没有任务在跑。
        result["jobs"]=jobSnapshot();
        return result;
    }
    if(method=="get_view_state") { refreshTextWarnings(); return viewState_; }
    if(method=="set_formula" || method=="export_animation" || method=="get_job" || method=="cancel_job") {
        try {
            auto result=jobRequest(request,source);
            recordActivityLine(request,source,true,std::string(),std::string());
            return result;
        } catch(const std::exception& e) {
            recordActivityLine(request,source,false,e.what(),std::string());
            throw;
        }
    }
    if(method=="show_editor") { showRequested_=true; return {{"showRequested",true}}; }
    if(source=="mcp" && followMcp_ && (method=="apply" || method=="replace" || method=="load" || method=="undo" || method=="redo" || method=="playback")) showRequested_=true;
    // Formula assets are part of this renderer's capabilities. Core remains GPU/TeX independent.
    if(method=="get_formulas") {
        Json result=Json::array(),latex=Json::object();
        for(const auto& e:workspace_.resources()["formulaMasks"]){result.push_back(e["name"]);if(e.contains("latex"))latex[e["name"].get<std::string>()]=e["latex"];}
        return {{"assets",result},{"latex",latex},{"note","Built-in and portable runtime LaTeX assets; use set_formula, then poll get_job."}};
    }
    Json result;
    try {
        result=workspace_.dispatch(request,source);
    } catch(const std::exception& e) {
        // 被拒绝的操作也必须出现在日志里：失败往往比成功更值得追查。
        recordActivityLine(request,source,false,e.what(),activityResolvedScope(method,scope_));
        throw;
    }
    if(method=="get_history" || method=="get_scene" || method=="get_catalog" || method=="get_timeline" || method=="get_project" || method=="sample_animation") return result;
    if(method=="load_project") { syncFormulaResources();applyLayout(); fitRequested_=false; lastCanvasSize_={};
        // 操作日志是「本会话」记录，不属于工程：打开另一个工程时清空，避免把
        // 上一个工程的旧操作误读成当前工程发生过的事。
        activityLog_.clear(); activityAfter_=0; activityLastId_=0; activityNewestSeen_=0; }
    if(method=="load_project"){++workspaceGeneration_;++componentGeneration_;}
    if(method=="open_component" || method=="close_component")++componentGeneration_;
    if(method=="save_project" || method=="load_project") strcpy_s(projectPath_,request["args"]["path"].get_ref<const std::string&>().c_str());
    const std::string target=(method=="load_project" || method=="save_project")?scope_:result.value("scope",std::string("scene"));
    if(source!="mcp" || followMcp_) { switchScope(target); if(source=="mcp") showRequested_=true; }
    if(method=="close_component") switchScope("scene");
    // A recorded export camera belongs to one document generation. Loading a
    // project, importing/replacing a document or swapping the draft invalidates
    // it so "current" can never silently export a stale or foreign scope.
    if(method=="load_project") invalidateRecordedViews();
    if(method=="load" || method=="replace") invalidateRecordedViews(target);
    if(method=="open_component" || method=="close_component") invalidateRecordedViews("component");
    dirty_=workspace_.dirty();
    if(method=="load" || method=="replace" || method=="open_component") fitRequested_=true;
    // A newly imported or replaced scene has no stored pixel baseline: the fit
    // performed below becomes the reference the text/formula sizes refer to.
    if((method=="load" || method=="replace") && target=="scene") referenceZoom_=0.f;
    if(method=="save" || method=="load") {
        const auto path=request.at("args").at("path").get<std::string>();
        if(path.size()<sizeof(path_)) { strcpy_s(target=="scene"?scenePath_:componentPath_,sizeof(path_),path.c_str()); if(scope_==target) strcpy_s(path_,path.c_str()); }
    }
    status_="完成："+method+" · 修订 "+std::to_string(active().revision());
    const bool journalError=!active().journalError().empty();
    if(journalError) status_+=" · "+active().journalError();
    recordActivityLine(request,source,true,journalError?active().journalError():std::string(),
        activityResolvedScope(method,target));
    return result;
}
void Editor::attempt(const Json& request) {
    auto scoped=request; if(!scoped.contains("scope")) scoped["scope"]=scope_;
    // dispatch() 已经在原地记录了成败；这里只兜住「请求还没进入 dispatch 就校验失败」
    // 的入口错误（例如请求缺少 method 字段），避免失败被静默吞掉。
    const uint64_t before=activityLog_.lastId();
    try { dispatch(scoped); }
    catch(const std::exception& e) {
        status_=std::string("操作失败：")+e.what();
        if(activityLog_.lastId()==before)
            activityLog_.record(scoped.value("method",std::string("invalid")),scope_,"human",false,
                ActivityLog::describeRequest(scoped),e.what(),Json{{"entry","request"}});
    }
}
bool Editor::requestClose() { closeDialog_=workspace_.projectDirty(); return !closeDialog_; }
void Editor::execute(Json commands) { attempt({{"method","apply"},{"args",{{"commands",commands}}}}); }
void Editor::palette() {
    if(ImGui::Button("添加图元…"))ImGui::OpenPopup("add-primitive");ImGui::SameLine();
    if(ImGui::Button("+ 圆")) {
        std::string id;do{id="circle_"+std::to_string(nextId_++);}while(std::any_of(active().scene()["objects"].begin(),active().scene()["objects"].end(),[&](const Json& o){return o["id"]==id;}));
        execute(Json::array({{{"op","create"},{"id",id},{"type","circle"},{"name","圆"},{"properties",{{"x",3},{"y",5}}}}}));selected_=id;
    }
    {const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();circleButton_={(a.x+b.x)/2,(a.y+b.y)/2};}
    if(ImGui::BeginPopup("add-primitive")) {
    for(const auto& p:active().registry().describe()) {
        const auto type=p["type"].get<std::string>();
        if(type=="component") continue;
        if(ImGui::Button(p["label"].get_ref<const std::string&>().c_str(),{94,30})) {
            std::string id;
            do { id=type+"_"+std::to_string(nextId_++); } while(std::any_of(active().scene()["objects"].begin(),active().scene()["objects"].end(),[&](const Json& o){return o["id"]==id;}));
            execute(Json::array({{{"op","create"},{"id",id},{"type",type},{"name",p["label"]},{"properties",{{"x",3},{"y",5}}}}})); selected_=id;
        }

        if(type=="circle" || type=="ground" || type=="point" || type=="rectangle") ImGui::SameLine();
    }
        ImGui::EndPopup();
    }
    ImGui::Separator(); ImGui::TextUnformatted("场景对象（后面的覆盖前面的）");
    std::string remove;
    for(const auto& o:active().scene()["objects"]) {
        const auto id=o["id"].get<std::string>(); ImGui::PushID(id.c_str());
        if(scope_=="scene") { bool chosen=grouped_.count(id)>0; if(ImGui::Checkbox("##group",&chosen)) { if(chosen) grouped_.insert(id); else grouped_.erase(id); } ImGui::SameLine(); }
        const std::string label=(o["visible"].get<bool>()?"":"[隐藏] ")+o["name"].get<std::string>();
        if(ImGui::Selectable(label.c_str(),selected_==id)) selected_=id;
        ImGui::PopID();
    }
    ImGui::Separator();
    if(scope_=="scene") componentPanel();
    else if(ImGui::Button("定义与公开参数")) selected_.clear();
    ImGui::TextWrapped("左键选择 / 拖动原点\n中键平移 · 滚轮缩放\n拖动时暂停动画\nCtrl+Z / Ctrl+Y 撤销重做");
}
void Editor::inspector() {
    ImGui::TextUnformatted("属性");
    const Json* object=nullptr; for(const auto& o:active().scene()["objects"]) if(o["id"]==selected_) object=&o;
    if(!object) { if(colorEditing_)cancelColorEdit("颜色编辑已取消：对象已不存在。"); if(stepEditing_)cancelStepEdit("出现步骤编辑已取消：对象已不存在。");
        if(scope_=="component") componentPanel(); else ImGui::TextWrapped("从画布或上方对象列表选择一个对象。"); return; }
    // Work on a copy: commands may replace the entire document on commit.
    const Json o=*object; const Json p=o["properties"];
    if(o["type"]=="formula")formulaPanel(o);
    if(o["type"]=="text" || o["type"]=="formula")writePanel(o);
    Json displayedProperties=p;
    if(scope_=="component") for(const auto& child:components::materialize(active().scene())) if(child["id"]==selected_) displayedProperties=child["properties"];
    ImGui::Text("ID: %s",selected_.c_str()); ImGui::Text("类型: %s",o["type"].get_ref<const std::string&>().c_str());
    auto patch=[&](Json value) { execute(Json::array({{{"op","update"},{"id",selected_},{"patch",value}}})); };
    char name[512]; strcpy_s(name,o["name"].get_ref<const std::string&>().c_str());
    if(ImGui::InputText("名称",name,sizeof(name),ImGuiInputTextFlags_EnterReturnsTrue)) patch({{"name",name}});
    bool visible=o["visible"], locked=o["locked"];
    if(ImGui::Checkbox("显示",&visible)) patch({{"visible",visible}}); ImGui::SameLine();
    if(ImGui::Checkbox("锁定拖动",&locked)) patch({{"locked",locked}});
    // Step edits preview live but commit exactly once on release. The conflict
    // baseline is captured when the gesture starts (mouse down), not at the
    // first value change, so an MCP edit between press and first move cancels
    // the gesture instead of being silently overwritten.
    const std::string stepTarget=scope_+":"+selected_;
    if(stepEditing_ && stepEditTarget_!=stepTarget) cancelStepEdit("出现步骤编辑已取消：选择或范围已切换。");
    int step=stepEditing_ && stepEditTarget_==stepTarget?stepPreview_:o["step"].get<int>();
    const bool stepChanged=ImGui::SliderInt("出现步骤",&step,0,active().scene()["steps"].get<int>()-1);
    if(!stepEditing_ && (stepChanged || ImGui::IsItemActivated())) beginStepEdit();
    if(stepChanged) stepPreview_=step;
    if(stepEditing_ && stepEditTarget_==stepTarget) {
        if(active().revision()!=stepEditRevision_) cancelStepEdit("出现步骤编辑已取消：文档已被其他操作更新，未覆盖对方修改。");
        else if(ImGui::IsItemDeactivatedAfterEdit()) commitStepEdit();
    }
    ImGui::SeparatorText("位置与形状");
    for(const char* key:{"x","y","dx","dy","radius","width","height","stroke","size","scale","start","end","rotation"}) if(p.contains(key)) {
        if(o["type"]=="component" && std::strcmp(key,"stroke")==0) continue;
        bool bound=o.contains("animation") && o["animation"]["channels"].contains(key);
        if(scope_=="component") for(const auto& b:active().scene()["componentDefinition"]["bindings"]) if(b["object"]==selected_ && b["property"]==key) bound=true;
        ImGui::BeginDisabled(bound); double value=displayedProperties[key]; if(inputNumber(key,&value)) patch({{"properties",{{key,value}}}}); ImGui::EndDisabled();
        if(bound) { ImGui::SameLine(); ImGui::TextDisabled(o.contains("animation")?"动画驱动":"参数驱动"); }
        else if(scope_=="component") { ImGui::SameLine(); ImGui::PushID(key);
            if(ImGui::SmallButton("公开")) {
                auto meta=active().scene()["componentDefinition"]; std::string parameterName;
                do { parameterName="p"+std::to_string(nextId_++); } while(meta["parameters"].contains(parameterName));
                meta["parameters"][parameterName]={{"default",value},{"min",value},{"max",value},{"unit",""}};
                meta["bindings"].push_back({{"object",selected_},{"property",key},{"expression",parameterName}});
                execute(Json::array({{{"op","component_settings"},{"patch",meta}}}));
            } ImGui::PopID();
        }
    }
    if(o["type"]=="component") {
        ImGui::SeparatorText("实例公开参数");
        const auto specs=active().scene()["components"][p["definition"].get<std::string>()]["componentDefinition"]["parameters"];
        for(auto it=specs.begin();it!=specs.end();++it) {
            double value=p["parameters"].value(it.key(),it.value()["default"].get<double>());
            const std::string label=it.key()+" / "+it.value().value("unit",std::string());
            if(inputNumber(label.c_str(),&value)) patch({{"properties",{{"parameters",{{it.key(),value}}}}}});
        }
    }
    if(o["type"]!="component") {
    const std::string colorTarget=scope_+":"+selected_;
    if(colorEditing_ && colorEditTarget_!=colorTarget) cancelColorEdit("颜色编辑已取消：选择或范围已切换。");
    const bool editingColor=colorEditing_ && colorEditTarget_==colorTarget;
    float c[4];
    if(editingColor) std::copy(colorPreview_,colorPreview_+4,c);
    else { c[0]=p["color"][0]; c[1]=p["color"][1]; c[2]=p["color"][2]; c[3]=p["color"][3]; }
    const bool colorChanged=ImGui::ColorEdit4("颜色",c,ImGuiColorEditFlags_NoInputs);
    // U3 smoke: remember where the real colour button is and, when its picker
    // popup is open, where that popup actually is. Both come from the live
    // widgets, so smokeInput never has to guess screen coordinates. Scrolling the
    // widget into view is smoke-only; normal editing is untouched.
    if(smokeUiArmed_) {
        const auto itemMin=ImGui::GetItemRectMin(),itemMax=ImGui::GetItemRectMax();
        // NoInputs places the square at the group's left edge; the group also
        // contains a label, so its centre need not hit the clickable square.
        smokeColorCenter_={itemMin.x+ImGui::GetFrameHeight()*.5f,(itemMin.y+itemMax.y)*.5f};
        smokeColorReady_=true;
        ImGui::SetScrollHereY(0.5f);
        ImGuiContext* context=ImGui::GetCurrentContext();
        smokePickerReady_=false;
        if(context!=nullptr && context->OpenPopupStack.Size>0) {
            ImGuiWindow* popup=context->OpenPopupStack.back().Window;
            if(popup!=nullptr) { smokePickerMin_=popup->Pos; smokePickerSize_=popup->Size; smokePickerReady_=true; }
        }
    }
    // Capture the baseline at gesture activation (popup open / press), not at the
    // first value change: an MCP edit between those moments must cancel the
    // gesture rather than be overwritten by an apparently fresh preview.
    if(!colorEditing_ && (colorChanged || ImGui::IsItemActivated())) beginColorEdit();
    if(colorChanged) std::copy(c,c+4,colorPreview_);
    if(colorEditing_ && colorEditTarget_==colorTarget) {
        if(active().revision()!=colorEditRevision_) cancelColorEdit("颜色编辑已取消：文档已被其他操作更新，未覆盖对方修改。");
        else if(ImGui::IsItemDeactivatedAfterEdit()) commitColorEdit();
    }
    }
    if(p.contains("text")) { char text[8192]; strcpy_s(text,displayedProperties["text"].get_ref<const std::string&>().c_str());
        bool bound=false; if(scope_=="component") for(const auto& b:active().scene()["componentDefinition"]["bindings"]) if(b["object"]==selected_ && b["property"]=="text") bound=true;
        ImGui::BeginDisabled(bound);
        if(ImGui::InputText("文字",text,sizeof(text),ImGuiInputTextFlags_EnterReturnsTrue)) patch({{"properties",{{"text",text}}}});
        ImGui::EndDisabled(); if(bound) ImGui::TextDisabled("文字由参数模板生成");
    }
    if(p.contains("formula")) {
        if(ImGui::BeginCombo("公式资源",p["formula"].get_ref<const std::string&>().c_str())) {
            for(const auto& entry:workspace_.resources()["formulaMasks"])if(ImGui::Selectable(entry["name"].get_ref<const std::string&>().c_str(),p["formula"]==entry["name"]))patch({{"properties",{{"formula",entry["name"]}}}});
            ImGui::EndCombo();
        }
        ImGui::TextWrapped("使用预编译 LaTeX 公式库；新增表达式见公式文档。");
    }
    ImGui::BeginDisabled(scope_=="component" || o.contains("animation")); ImGui::SeparatorText("运动与绑定");
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
            for(const auto& other:active().scene()["objects"]) if(other["id"]!=selected_ && !other.contains("binding")) {
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
    ImGui::EndDisabled();
    if(ImGui::Button("删除对象")) execute(Json::array({{{"op","delete"},{"id",selected_}}}));
    ImGui::SameLine(); if(ImGui::Button("置于最上层")) execute(Json::array({{{"op","reorder"},{"id",selected_},{"index",active().scene()["objects"].size()-1}}}));
    if(ImGui::CollapsingHeader("完整属性 JSON / 扩展字段")) {
        if(editedId_!=selected_ || editedRevision_!=active().displayRevision()) {
            Json editable=o; editable.erase("id"); editable.erase("type"); const auto text=editable.dump(2);
            if(text.size()<sizeof(jsonEdit_)) strcpy_s(jsonEdit_,text.c_str()); else strcpy_s(jsonEdit_,"{}");
            editedId_=selected_; editedRevision_=active().displayRevision();
        }
        ImGui::InputTextMultiline("##json",jsonEdit_,sizeof(jsonEdit_),{-1,230});
        if(ImGui::Button("应用 JSON 属性")) { try { patch(Json::parse(jsonEdit_)); } catch(const std::exception& e) { status_=e.what(); } }
    }
    ImGui::TextWrapped("数值 / 文字输入后按 Enter 提交。x、y 是初始位置；绑定时是相对偏移。");
}
void Editor::canvas(ImVec2 size) {
    drawnRevision_=active().displayRevision(); drawnTime_=active().time(); drawnScope_=scope_;
    const auto start=ImGui::GetCursorScreenPos();
    // The child owns the available content region; never force a minimum that
    // would overflow it in a small window. Arithmetic below tolerates 1 px.
    size.x=std::max(size.x,1.f); size.y=std::max(size.y,1.f);
    lastCanvasStart_=start;
    if(lastCanvasSize_.x>0 && (size.x!=lastCanvasSize_.x || size.y!=lastCanvasSize_.y)) fitRequested_=true;
    lastCanvasSize_=size;
    refreshTextWarnings();
    ImGui::InvisibleButton("canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered=ImGui::IsItemHovered(); auto& io=ImGui::GetIO(); auto* draw=ImGui::GetWindowDrawList();
    origin_={start.x+size.x*.25f+pan_.x,start.y+size.y*.85f+pan_.y};
    if(hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) { pan_.x+=io.MouseDelta.x; pan_.y+=io.MouseDelta.y; origin_.x+=io.MouseDelta.x; origin_.y+=io.MouseDelta.y; }
    if(hovered && io.MouseWheel!=0) { const auto before=world(io.MousePos); zoom_=std::clamp(zoom_*std::pow(1.1f,io.MouseWheel),8.f,1200.f);
        const auto after=screen(before.x,before.y); pan_.x+=io.MousePos.x-after.x; pan_.y+=io.MousePos.y-after.y; origin_.x+=io.MousePos.x-after.x; origin_.y+=io.MousePos.y-after.y;
    }
    draw->AddRectFilled(start,{start.x+size.x,start.y+size.y},IM_COL32(252,251,248,255),8);
    draw->PushClipRect(start,{start.x+size.x,start.y+size.y},true);
    if(grid_) {
        for(float x=std::fmod(origin_.x-start.x,zoom_);x<size.x;x+=zoom_) draw->AddLine({start.x+x,start.y},{start.x+x,start.y+size.y},IM_COL32(228,231,233,255));
        for(float y=std::fmod(origin_.y-start.y,zoom_);y<size.y;y+=zoom_) draw->AddLine({start.x,start.y+y},{start.x+size.x,start.y+y},IM_COL32(228,231,233,255));
    }
    Json displayObjects=scope_=="component"?components::materialize(active().scene()):active().scene()["objects"];
    if(fitRequested_) {
        try {
            // 文字与公式的标称像素基准（referenceZoom_）只在文档刚替换/载入
            // （referenceZoom_<=0）时由一次拟合确定。这次拟合必须按「编辑画布」的
            // 宽度进行：右侧操作日志是可收起、可拖动、可整栏让位的只读面板，若让它
            // 参与基准拟合，同一个文档在日志栏展开/收起时会得到不同的标称像素基准，
            // 而离屏导出（fit 视口）正是按这个基准缩放文字与公式的——那会变成
            // 「改布局就改导出像素」。这里先用编辑画布宽度求出基准，再按真实画布
            // 拟合一次显示用的缩放；两次都以 canvasScale()==1 计算，与没有日志栏时
            // 的算术完全一致。
            if(referenceZoom_<=0.f && editingCanvasWidth_>size.x+0.5f) {
                const ImVec2 savedPan=pan_,savedOrigin=origin_;
                fitCanvas(start,{editingCanvasWidth_,size.y},displayObjects);
                const float baseline=referenceZoom_>0.f?referenceZoom_:zoom_;
                pan_=savedPan;origin_=savedOrigin;zoom_=baseline;   // 真实画布按基准比例拟合
                referenceZoom_=baseline;
            }
            fitCanvas(start,size,displayObjects);
        } catch(const std::exception& e) { status_=e.what(); }
        fitRequested_=false;
    }
    std::string hit;
    for(auto o:displayObjects) {
        try {
        o["properties"]=animation::properties(o,active().time(),active().scene()["duration"]);
        active().registry().get(o["type"]).validate(o["properties"]);
        const auto id=o["id"].get<std::string>();
        // Temporary in-place previews: the document is untouched until release.
        int objectStep=o["step"].get<int>();
        if(stepEditing_ && stepEditTarget_==scope_+":"+id) objectStep=stepPreview_;
        if(!o["visible"].get<bool>() || objectStep>active().step()) continue;
        auto state=scope_=="component"?State{o["properties"]["x"],o["properties"]["y"],0,0}:active().evaluate(id,active().time());
        if(dragging_ && id==selected_) { state.x+=dragPreview_.x; state.y+=dragPreview_.y; }
        if(trails_ && o.contains("motion")) {
            ImVec2 previous=screen(active().evaluate(id,0).x,active().evaluate(id,0).y);
            for(int i=1;i<=100;++i) { const auto v=active().evaluate(id,active().time()*i/100.0); const auto q=screen(v.x,v.y); draw->AddLine(previous,q,IM_COL32(153,180,197,140),std::max(0.75f,1.5f*canvasScale())); previous=q; }
        }
        const auto renderer=renderers_.find(o["type"].get<std::string>());
        const ImU32 tint=(colorEditing_ && colorEditTarget_==scope_+":"+id)
            ?ImGui::ColorConvertFloat4ToU32({colorPreview_[0],colorPreview_[1],colorPreview_[2],colorPreview_[3]})
            :color(o["properties"]);
        if(renderer!=renderers_.end()) renderer->second(draw,o,state,tint);
        const auto anchor=screen(state.x,state.y);
        // Every primitive exposes the same draggable anchor, including text/formula.
        bool contains=distance(io.MousePos,anchor)<16;
        const auto& p=o["properties"]; const std::string type=o["type"];
        if(type=="circle") contains=contains || std::abs(distance(io.MousePos,anchor)-f(p,"radius")*zoom_)<8;
        if(type=="point") contains=contains || distance(io.MousePos,anchor)<f(p,"radius")*zoom_+5;
        if(type=="text" || type=="formula" || type=="rectangle") {
            ImVec2 extent{}; ImVec2 top=anchor;
            if(type=="text") { extent=ImGui::GetFont()->CalcTextSizeA(f(p,"size")*canvasScale(),1e6f,0,p["text"].get_ref<const std::string&>().c_str()); }
            else if(type=="formula") extent=math_text::size(p["formula"].get_ref<const std::string&>().c_str(),f(p,"scale")*canvasScale());
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
        if(type=="arc") {
            double angle=std::atan2(anchor.y-io.MousePos.y,io.MousePos.x-anchor.x)*180/3.141592653589793;
            double offset=std::fmod(angle-p["start"].get<double>()+3600000,360.0);
            contains=contains || (offset<=p["end"].get<double>()-p["start"].get<double>() && std::abs(distance(io.MousePos,anchor)-f(p,"radius")*zoom_)<8);
        }
        if(type=="component" || type=="rectangle") {
            const auto b=bounds(o,state); const auto q=world(io.MousePos);
            contains=contains || (q.x>=b.first.x && q.x<=b.second.x && q.y>=b.first.y && q.y<=b.second.y);
            if(id==selected_) draw->AddRect(screen(b.first.x,b.second.y),screen(b.second.x,b.first.y),IM_COL32(237,149,48,150));
        }
        if(contains) hit=id;
        if(id==selected_) {
            draw->AddCircle(anchor,12,IM_COL32(237,149,48,255),24,2);
            if(type=="point" || type=="circle" || type=="rectangle") draw->AddText({anchor.x+16,anchor.y+10},IM_COL32(163,99,25,255),o["name"].get_ref<const std::string&>().c_str());
        }
        } catch(const std::exception& e) {
            active().dispatch({{"method","playback"},{"args",{{"playing",false}}}},"preview");
            status_="动画求值失败（已暂停）："+o["id"].get<std::string>()+" · "+e.what();
        }
    }
    draw->AddText({start.x+16,start.y+12},IM_COL32(105,118,129,255),"运动示意画布 · +y 向上 · 箭头使用独立比例");
    if(!textWarnings_.empty()) {
        // Authoring warning overlay only: no document edit, no history, and it
        // never replaces the task status line.
        const std::string first=textWarnings_[0].value("object",std::string("?"));
        char label[192]; std::snprintf(label,sizeof(label),"字体缺字 %zu 处 · 首个对象 %s",
            textWarnings_.size(),first.c_str());
        const auto extent=ImGui::CalcTextSize(label);
        const ImVec2 pos{start.x+size.x-extent.x-14,start.y+12};
        draw->AddRectFilled({pos.x-8,pos.y-4},{pos.x+extent.x+8,pos.y+extent.y+4},IM_COL32(70,24,18,210),4);
        draw->AddText(pos,IM_COL32(255,138,102,255),label);
    }
    if(hovered && ImGui::IsMouseClicked(0)) {
        if(!hit.empty()) { selected_=hit; const auto& o=active().object(hit);
            bool boundPosition=o.contains("animation") && (o["animation"]["channels"].contains("x") || o["animation"]["channels"].contains("y"));
            if(scope_=="component") for(const auto& b:active().scene()["componentDefinition"]["bindings"]) if(b["object"]==hit && (b["property"]=="x" || b["property"]=="y")) boundPosition=true;
            if(!active().previewing() && !boundPosition && !o["locked"].get<bool>()) { if(active().playing()) active().dispatch({{"method","playback"},{"args",{{"playing",false}}}},"human"); dragging_=true; dragStart_=io.MousePos; dragPreview_={}; dragRevision_=active().revision(); }
        } else selected_.clear();
    }
    if(dragging_) {
        if(active().revision()!=dragRevision_) { dragging_=false; status_="拖动已取消：场景被其他操作更新。"; }
        else {
            dragPreview_={(io.MousePos.x-dragStart_.x)/zoom_,(dragStart_.y-io.MousePos.y)/zoom_};
            if(ImGui::IsMouseReleased(0)) { dragging_=false; if(distance(dragPreview_,{})>.001f) execute(Json::array({{{"op","move"},{"id",selected_},{"dx",dragPreview_.x},{"dy",dragPreview_.y}}})); }
        }
    }
    draw->PopClipRect();
    publishFrameView();
}
void Editor::draw(float dt) {
    if(!styleReady_) {
        auto& style=ImGui::GetStyle(); ImGui::StyleColorsDark();
        style.WindowPadding={8,6};style.FramePadding={6,3};style.ItemSpacing={6,5};
        style.WindowRounding=0;style.ChildRounding=0;style.FrameRounding=2;style.ScrollbarRounding=2;
        style.Colors[ImGuiCol_WindowBg]={.12f,.12f,.12f,1};style.Colors[ImGuiCol_ChildBg]={.15f,.15f,.15f,1};
        style.Colors[ImGuiCol_Button]={.24f,.24f,.24f,1};style.Colors[ImGuiCol_FrameBg]={.09f,.09f,.09f,1};
        style.Colors[ImGuiCol_Header]={.28f,.37f,.46f,1};style.Colors[ImGuiCol_CheckMark]={.95f,.58f,.22f,1};
        styleReady_=true;
    }
    // 本帧后台任务进度的呈现位置由这一帧的实际绘制决定（日志栏内 / 兜底浮层 /
    // 没画）；每帧先清空，再由真正画了进度的地方写回。
    jobsSurface_="none";
    active().advance(dt); dirty_=workspace_.projectDirty(); auto* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("物理示意编辑器",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_MenuBar);
    if(ImGui::BeginMenuBar()) {
        if(ImGui::BeginMenu("文件")) {
            if(ImGui::MenuItem("新建空白场景")) {auto empty=Document::demo();empty["objects"]=Json::array();empty["title"]="未命名";attempt({{"method","replace"},{"scope","scene"},{"args",{{"document",empty}}}});}
            if(ImGui::MenuItem("打开 / 另存工程…"))projectDialogOpen_=true;
            if(ImGui::MenuItem("导出动画…"))exportDialogOpen_=true;
            if(ImGui::MenuItem("保存工程","Ctrl+S"))attempt({{"method","save_project"},{"args",{{"path",projectPath_},{"overwrite",true}}}});
            ImGui::Separator(); ImGui::SetNextItemWidth(330);ImGui::InputText("JSON 路径",path_,sizeof(path_));
            if(ImGui::MenuItem("导入当前文档 JSON"))attempt({{"method","load"},{"args",{{"path",path_}}}});
            if(ImGui::MenuItem("导出当前文档 JSON"))attempt({{"method","save"},{"args",{{"path",path_},{"overwrite",true}}}});
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("编辑")) { if(ImGui::MenuItem("撤销","Ctrl+Z"))attempt({{"method","undo"}});if(ImGui::MenuItem("重做","Ctrl+Y"))attempt({{"method","redo"}});ImGui::EndMenu(); }
        if(ImGui::BeginMenu("场景")) {
            ImGui::BeginDisabled(active().previewing());double steps=active().scene()["steps"].get<double>();ImGui::SetNextItemWidth(120);
            if(inputNumber("步骤数",&steps) && steps==std::floor(steps) && steps>=1 && steps<=100)execute(Json::array({{{"op","settings"},{"steps",int(steps)}}}));
            ImGui::EndDisabled();ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("视图")) {
            if(ImGui::MenuItem("适应画布"))fitRequested_=true;
            ImGui::MenuItem("网格",nullptr,&grid_); ImGui::MenuItem("轨迹",nullptr,&trails_);
            ImGui::Separator();
            if(ImGui::MenuItem("以当前缩放重设文字/公式基准"))resetPixelReference(zoom_);
            ImGui::TextDisabled("缩放 %.1f px/m · 基准 %.1f px/m · 整体比例 ×%.2f",
                zoom_,referenceZoom_>0.f?referenceZoom_:zoom_,canvasScale());
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("示例")) {
            if(ImGui::MenuItem("斜面接圆弧"))attempt({{"method","load"},{"scope","scene"},{"args",{{"path","scenes/incline-arc.json"}}}});
            if(ImGui::MenuItem("上抛运动"))attempt({{"method","replace"},{"scope","scene"},{"args",{{"document",Document::demo()}}}});
            if(ImGui::MenuItem("公式与阶跃动画")) { attempt({{"method","replace"},{"scope","scene"},{"args",{{"document",animation::demo()}}}});selected_="block";channel_="y";animationWorkspace_=true;historyOpen_=false;fitRequested_=true; }
            if(ImGui::MenuItem("手写入场（中文 + 公式）")) { attempt({{"method","replace"},{"scope","scene"},{"args",{{"document",write_entry::demo()}}}});selected_="note";animationWorkspace_=false;historyOpen_=false;fitRequested_=true; }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if(ImGui::MenuItem("布局",nullptr,!animationWorkspace_)){animationWorkspace_=false;historyOpen_=false;}
        if(ImGui::MenuItem("动画",nullptr,animationWorkspace_)){animationWorkspace_=true;historyOpen_=false;}
        ImGui::TextDisabled("   %s%s",active().scene()["title"].get_ref<const std::string&>().c_str(),dirty_?" *":"");
        ImGui::EndMenuBar();
    }
    if(ImGui::Button(scope_=="scene"?"[主场景]":"主场景"))switchScope("scene");ImGui::SameLine();
    if(workspace_.hasComponent()) {
        if(ImGui::Button(scope_=="component"?"[组件草稿]":"组件草稿"))switchScope("component");ImGui::SameLine();
        if(ImGui::Button("关闭草稿"))attempt({{"method","close_component"},{"args",{{"discard",true}}}});ImGui::SameLine();
    }
    if(ImGui::Button(historyOpen_?"[编辑历史]":"编辑历史"))historyOpen_=!historyOpen_;ImGui::SameLine();
    // 操作日志是默认可见的右侧额外侧栏；F9 与按钮等效。
    if(ImGui::Button(activityOpen_?"[操作日志]":"操作日志"))activityOpen_=!activityOpen_;ImGui::SameLine();
    ImGui::Checkbox("跟随 MCP",&followMcp_);ImGui::SameLine();ImGui::Checkbox("网格",&grid_);ImGui::SameLine();ImGui::Checkbox("轨迹",&trails_);ImGui::SameLine();
    if(ImGui::Button("适应画布"))fitRequested_=true;
    auto& io=ImGui::GetIO();if(!io.WantTextInput && !ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId)) {
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z,false))attempt({{"method","undo"}});
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y,false))attempt({{"method","redo"}});
        if(io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S,false))attempt({{"method","save_project"},{"args",{{"path",projectPath_},{"overwrite",true}}}});
        if(ImGui::IsKeyPressed(ImGuiKey_Space,false))attempt({{"method","playback"},{"args",{{"playing",!active().playing()}}}});
        if(ImGui::IsKeyPressed(ImGuiKey_F9,false))activityOpen_=!activityOpen_;
    }
    const ImVec2 content=ImGui::GetContentRegionAvail();
    const float width=std::max(content.x,1.f),contentHeight=std::max(content.y,1.f);
    const bool expanded=historyOpen_ || animationWorkspace_;
    // Children are clamped to the real content region: a small window must not
    // get panels wider/taller than it has, and every child keeps scrolling.
    const float toolbarReserve=92.f;
    float bottom=0.f;
    if(expanded) {
        const float maxBottom=contentHeight-toolbarReserve-120.f;
        if(maxBottom>=80.f) bottom=std::clamp(bottomHeight_,std::min(120.f,maxBottom),maxBottom);
    }
    const float height=std::min(std::max(contentHeight-toolbarReserve-bottom,1.f),contentHeight);
    // 三列：左图元/属性、中画布、右操作日志。列宽由 computeEditorColumns 统一
    // 裁剪（纯函数，可单独测试）：窄窗口先牺牲只读日志栏（按钮条 → 整栏让位），
    // 再压缩承载编辑功能的左栏，画布始终 >=160 px，不会出现负宽度或把画布/侧栏
    // 挤出屏幕。
    const EditorColumns columns=computeEditorColumns(width,sidebarWidth_,activityWidth_,activityOpen_);
    sidebarWidth_=columns.sidebar;activityWidth_=columns.activity;
    // 文字/公式的标称像素基准只在需要重新确定时（刚替换/载入文档）使用这个宽度，
    // 它与日志栏的展开/收起/拖动无关，见 canvas()。
    editingCanvasWidth_=columns.editingDrawing;
    // 画布列的屏幕左上角：日志栏收起时的兜底进度浮层锚在这里（见 jobsFallbackOverlay）。
    const ImVec2 drawingOrigin=ImGui::GetCursorScreenPos();
    ImGui::BeginChild("drawing",{columns.drawing,height});canvas(ImGui::GetContentRegionAvail());ImGui::EndChild();
    if(columns.sidebar>0.f) {
        ImGui::SameLine(0,columns.sidebarSplitter*.5f);
        ImGui::InvisibleButton("##vertical-splitter",{columns.sidebarSplitter,height});
        if(ImGui::IsItemActive())sidebarWidth_=activityClampSidebar(sidebarWidth_-io.MouseDelta.x);
        if(ImGui::IsItemHovered())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SameLine(0,columns.sidebarSplitter*.5f);
        ImGui::BeginChild("sidebar",{columns.sidebar,height});
        const float paletteHeight=std::max(1.f,std::min(height*.45f,std::max(1.f,height-60.f)));
        ImGui::BeginChild("palette",{0,paletteHeight},ImGuiChildFlags_Borders);ImGui::BeginDisabled(active().previewing());palette();ImGui::EndDisabled();ImGui::EndChild();
        ImGui::BeginChild("properties",{0,0},ImGuiChildFlags_Borders);ImGui::BeginDisabled(active().previewing());inspector();ImGui::EndDisabled();ImGui::EndChild();ImGui::EndChild();
    }
    if(columns.activityShown) {
        if(columns.activitySplitter>0.f) {
            ImGui::SameLine(0,columns.activitySplitter*.5f);
            ImGui::InvisibleButton("##activity-splitter",{columns.activitySplitter,height});
            if(ImGui::IsItemActive())activityWidth_=std::clamp(activityWidth_-io.MouseDelta.x,240.f,700.f);
            if(ImGui::IsItemHovered())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SameLine(0,columns.activitySplitter*.5f);
        } else {
            // A collapsed log still belongs to the same row as the canvas.
            ImGui::SameLine(0,6.f);
        }
        // 日志栏必须和左栏一样被限制在 {宽度, 画布高度} 的子窗口里：栏内的列表用
        // 高度 0（填满剩余空间）绘制，如果直接在主窗口里画，它会一路吃到窗口底部，
        // 把下面的编辑历史/动画面板和状态行挤出可视区域（鼠标就再也点不到它们）。
        ImGui::BeginChild("activity",{columns.activity,height});
        activityPanel(columns);
        ImGui::EndChild();
    }
    if(expanded && bottom>0.f){ImGui::InvisibleButton("##horizontal-splitter",{-1,5});if(ImGui::IsItemActive())bottomHeight_-=io.MouseDelta.y;if(ImGui::IsItemHovered())ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);}
    ImGui::BeginDisabled(active().previewing());
    if(ImGui::Button(active().playing()?"暂停":"播放"))attempt({{"method","playback"},{"args",{{"playing",!active().playing()},{"time",active().time()>=active().scene()["duration"].get<double>()?0:active().time()}}}});ImGui::SameLine();
    if(ImGui::Button("|<"))attempt({{"method","playback"},{"args",{{"time",0},{"playing",false}}}});ImGui::SameLine();
    float time=static_cast<float>(active().time());ImGui::SetNextItemWidth(std::max(160.f,width*.35f));
    if(ImGui::SliderFloat("时间 / s",&time,0,active().scene()["duration"].get<float>(),"%.3f"))attempt({{"method","playback"},{"args",{{"time",std::min(double(time),active().scene()["duration"].get<double>())},{"playing",false}}}});
    ImGui::SameLine();int step=active().step();ImGui::SetNextItemWidth(120);if(ImGui::SliderInt("解题步骤",&step,0,active().scene()["steps"].get<int>()-1))attempt({{"method","playback"},{"args",{{"step",step}}}});
    double duration=active().scene()["duration"];ImGui::SameLine();ImGui::SetNextItemWidth(70);if(inputNumber("时长",&duration))execute(Json::array({{{"op","settings"},{"duration",duration}}}));
    ImGui::EndDisabled();
    if(historyOpen_ && bottom>0.f)historyPanel();else if(animationWorkspace_ && bottom>0.f){ImGui::BeginChild("animation-workspace",{0,bottom},ImGuiChildFlags_Borders);animationPanel();ImGui::EndChild();}
    if(active().previewing()){ImGui::TextColored({1,.6f,.15f,1},"历史预览 r%llu · 只读",(unsigned long long)active().displayRevision());ImGui::SameLine();if(ImGui::SmallButton("返回当前状态"))attempt({{"method","preview"}});}
    // 状态行是单行的：导出失败原因里常常带整条路径，直接画会被窗口右边缘裁掉。
    // 这里按可用宽度省略并给 tooltip（活动日志里的同一条错误另有换行显示）。
    {
        const std::string statusShown=elideUtf8ToWidth(status_,ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(statusShown.c_str());
        if(statusShown!=status_ && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",status_.c_str());
    }
    // 日志栏收起（F9）、只剩按钮条或整栏让位时，后台任务进度**不能**再画在这条
    // 正常布局流的末尾：画布高度已经吃满剩余空间，那几行会被状态行与播放控件挤出
    // 窗口底部，进度与「取消任务」就都点不到了（真实截图即如此）。落点由
    // jobPanelSurface() 统一决定（冒烟测试断言同一个函数）：日志栏画不下就改画在
    // jobsFallbackOverlay 的固定浮层里——它不参与主布局，画布尺寸/拟合基准/离屏
    // 导出取景都不受影响。
    if(std::string(jobPanelSurface(anyJobRunning(),columns.activityShown,columns.activityBar))=="overlay") {
        jobsFallbackOverlay(drawingOrigin,columns.drawing);
        jobsSurface_="overlay";
    }
    storeLayout();projectDialog();exportDialog();
    if(closeDialog_) { ImGui::OpenPopup("保存修改？"); closeDialog_=false; }
    if(ImGui::BeginPopupModal("保存修改？",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("工程有未保存的修改。保存工程会包含场景、草稿与历史。");
        if(ImGui::Button("保存并退出")) {
            attempt({{"method","save_project"},{"args",{{"path",projectPath_},{"overwrite",true}}}});
            if(!workspace_.projectDirty()) { quit_=true; ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine(); if(ImGui::Button("不保存退出")) { quit_=true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if(ImGui::Button("取消")) { smokeCloseCancelled_=true; ImGui::CloseCurrentPopup(); }
        const auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax(); closeCancelCenter_={(a.x+b.x)/2,(a.y+b.y)/2};
        ImGui::TextWrapped("%s",status_.c_str());
        ImGui::EndPopup();
    }
    ImGui::End();
}
void Editor::historyPanel() {
    ImGui::BeginChild("编辑历史 · 独立于运动时间",{0,std::max(1.f,std::min(bottomHeight_,ImGui::GetContentRegionAvail().y))},ImGuiChildFlags_Borders);
    ImGui::Text("当前历史：%s · 显示 r%llu / 当前 r%llu · 保留最近 101 个状态",scope_=="scene"?"主场景":"组件定义草稿",(unsigned long long)active().displayRevision(),(unsigned long long)active().revision());
    const auto timeline=active().timeline(); const auto states=timeline["states"];
    int cursor=0; for(size_t i=0;i<states.size();++i) if(states[i]["revision"]==active().displayRevision()) cursor=int(i);
    ImGui::SetNextItemWidth(-1);
    if(ImGui::SliderInt("##history-cursor",&cursor,0,int(states.size())-1,"历史状态 %d",ImGuiSliderFlags_AlwaysClamp))
        attempt({{"method","preview"},{"args",{{"revision",states[cursor]["revision"]}}}});
    historySliderStart_=ImGui::GetItemRectMin(); historySliderEnd_=ImGui::GetItemRectMax();
    if(active().previewing()) {
        if(ImGui::Button("退出历史预览")) attempt({{"method","preview"}}); ImGui::SameLine();
        if(ImGui::Button("恢复此状态（生成新操作）")) attempt({{"method","restore_preview"},{"expectedRevision",active().revision()}});
        ImGui::SameLine();
    }
    const auto page=active().history(historyAfter_,100);

    if(!page["journalError"].get_ref<const std::string&>().empty()) ImGui::TextColored({1,.45f,.3f,1},"%s",page["journalError"].get_ref<const std::string&>().c_str());
    if(ImGui::Button("最早记录")) historyAfter_=0; ImGui::SameLine();
    if(ImGui::Button("最近记录")) { const auto last=page["lastId"].get<uint64_t>(); historyAfter_=last>100?last-100:0; }
    ImGui::SameLine(); if(ImGui::Button("下一页") && page["hasMore"].get<bool>()) historyAfter_=page["nextCursor"];
    if(ImGui::BeginTable("events",5,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,{0,100})) {
        for(const char* label:{"序号","来源","操作","结果 / 修订","时间 UTC"}) ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        for(const auto& e:page["entries"]) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto id=std::to_string(e["id"].get<uint64_t>());
            if(ImGui::Selectable(id.c_str(),!historySelection_.is_null() && historySelection_["id"]==e["id"],ImGuiSelectableFlags_SpanAllColumns)) {
                historySelection_=e;
                if(e["status"]=="success" && e.value("documentChanged",false)) attempt({{"method","preview"},{"args",{{"revision",e["afterRevision"]}}}});
            }
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
    if(ImGui::CollapsingHeader("记录详情 / 磁盘日志")) { ImGui::TextWrapped("%s",page["journalPath"].get_ref<const std::string&>().c_str()); if(!historySelection_.is_null()) ImGui::TextUnformatted(historySelection_.dump(2).c_str()); }
    ImGui::EndChild();
}
// ---------------------------------------------------------------------------
// 右侧「操作日志」侧栏。它显示来自界面与 MCP 的实际操作（含范围、时间、成败），
// 与底部撤销历史互不影响：这里不回退文档，撤销历史也不分担操作记录。
// 折叠态只占一条 24 px 的展开按钮，宽窗口下也不会把画布挤没。
// ---------------------------------------------------------------------------
void Editor::activityPanel(EditorColumns columns) {
    activityVisible_=columns.activityShown;
    activityCollapsedBar_=columns.activityBar||!activityOpen_;
    activityFollowing_=false;
    if(!columns.activityShown) { activityVisible_=false; return; }
    // 折叠态：用户收起，或窗口太窄只放得下一条按钮条。点击按钮会在下一帧重新
    // 计算列宽并画出完整侧栏（窗口够宽时）。外层 child 已经把这一栏限制在
    // {columns.activity, 画布高度} 内，所以按钮直接填满它。
    if(!activityOpen_ || columns.activityBar) {
        if(ImGui::Button("日\n志",{-1,-1})) activityOpen_=true;
        if(ImGui::IsItemHovered()) ImGui::SetTooltip(activityOpen_
            ?"窗口太窄：日志栏只显示按钮条，拉宽窗口或收起左侧面板即可展开"
            :"展开操作日志（F9）");
        return;
    }
    const ImGuiStyle& style=ImGui::GetStyle();
    // 标题行：先明确这是「操作日志」（与底部可回退的「编辑历史」是两回事），再给
    // 收起入口。按钮用纯中文，不用 ▶ 之类的字符——交互式字体图集只覆盖 ASCII/
    // Latin-1/通用标点/CJK 与数学补充区，几何图形区（U+25xx）没有字形，真实截图里
    // 「收起 ▶」就渲染成了「收起 ?」。
    // 头部/筛选/进度都用 AutoResizeY 子窗口按内容取高：栏宽 240~340 px 时行内容
    // 放不下会换行，写死高度会把换行后的内容裁掉或压出一条滚动条。
    ImGui::BeginChild("activity-header",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
    const float headerWidth=ImGui::GetContentRegionAvail().x;
    const float titleWidth=ImGui::CalcTextSize("操作日志").x;
    const float collapseWidth=ImGui::CalcTextSize("收起日志").x+style.FramePadding.x*2.f;
    const float followWidth=ImGui::GetFrameHeight()+style.ItemInnerSpacing.x+ImGui::CalcTextSize("自动跟随").x;
    ImGui::TextUnformatted("操作日志");
    if(titleWidth+style.ItemSpacing.x+collapseWidth<=headerWidth) ImGui::SameLine();
    if(ImGui::SmallButton("收起日志")) activityOpen_=false;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("收起右侧日志栏（F9 重新展开）");
    if(titleWidth+collapseWidth+followWidth+style.ItemSpacing.x*2.f<=headerWidth) ImGui::SameLine();
    ImGui::Checkbox("自动跟随",&activityFollow_);
    // 变参只传 const char*：std::string 作为可变参数是不可移植的（C4840）。
    char countText[96]{};
    if(activityLog_.dropped())
        std::snprintf(countText,sizeof(countText),"最近 %zu / %zu 条 · 已淘汰 %llu 条",activityLog_.size(),
            ActivityLog::kCapacity,static_cast<unsigned long long>(activityLog_.dropped()));
    else std::snprintf(countText,sizeof(countText),"最近 %zu / %zu 条",activityLog_.size(),ActivityLog::kCapacity);
    const float clearWidth=ImGui::CalcTextSize("清空").x+style.FramePadding.x*2.f;
    ImGui::TextDisabled("%s",countText);
    if(ImGui::CalcTextSize(countText).x+style.ItemSpacing.x+clearWidth<=headerWidth) ImGui::SameLine();
    if(ImGui::SmallButton("清空")) { activityLog_.clear(); activityAfter_=0; activityLastId_=0; }
    ImGui::EndChild();

    // 后台任务进度紧跟标题：导出对话框关闭后它仍然可见，MCP 触发的任务也走同一份
    // state。AutoResizeY 让这一块只占内容高度，日志列表拿走剩下的空间。
    if(anyJobRunning()) {
        ImGui::BeginChild("activity-jobs",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
        ImGui::TextUnformatted("后台任务进度");
        jobsPanel();
        ImGui::EndChild();
    }
    // 走不到这里时（日志栏收起/整栏让位）由 draw() 的兜底浮层负责，两处用同一个判断。
    jobsSurface_=jobPanelSurface(anyJobRunning(),columns.activityShown,columns.activityBar);

    ImGui::BeginChild("activity-filter",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
    const float filterWidth=ImGui::GetContentRegionAvail().x;
    const auto checkWidth=[&](const char* label){return ImGui::GetFrameHeight()+style.ItemInnerSpacing.x+ImGui::CalcTextSize(label).x;};
    const float filterButtonWidth=ImGui::CalcTextSize("筛选…").x+style.FramePadding.x*2.f;
    const float filterChecksWidth=checkWidth("界面")+checkWidth("MCP")+checkWidth("失败")+style.ItemSpacing.x*2.f;
    // 三个勾选框在窄栏里并排放不下：整组换到下一行，而不是让「失败」被栏右边缘裁掉。
    const bool filterOneLine=filterButtonWidth+style.ItemSpacing.x+filterChecksWidth<=filterWidth;
    if(ImGui::Button("筛选…")) ImGui::OpenPopup("activity-filter-popup");
    if(filterOneLine) ImGui::SameLine(); ImGui::Checkbox("界面",&activityShowUi_);
    if(filterOneLine) ImGui::SameLine(); ImGui::Checkbox("MCP",&activityShowMcp_);
    if(filterOneLine) ImGui::SameLine(); ImGui::Checkbox("失败",&activityShowFailed_);
    if(ImGui::BeginPopup("activity-filter-popup")) {
        ImGui::Checkbox("人类界面 (human)",&activityShowUi_);
        ImGui::Checkbox("MCP (mcp)",&activityShowMcp_);
        ImGui::Checkbox("失败 / 警告",&activityShowFailed_);
        ImGui::Separator();
        ImGui::Checkbox("显示只读查询（默认静默）",&activityShowQuiet_);
        ImGui::TextDisabled("查询类请求成功时不记录，避免轮询刷屏；\n它们一旦失败会立刻出现。");
        ImGui::Separator();
        if(ImGui::Button("全选")) { activityShowUi_=activityShowMcp_=activityShowFailed_=true; }
        ImGui::SameLine(); if(ImGui::Button("仅失败")) { activityShowUi_=activityShowMcp_=false; activityShowFailed_=true; }
        ImGui::EndPopup();
    }
    // 这里必须换行（TextWrapped）：240~340 px 的栏宽下这一句以前被栏右边缘裁掉一半。
    ImGui::TextWrapped("界面 / MCP 使用同一元操作，日志格式一致；此处只显示，不影响撤销历史。");
    ImGui::EndChild();

    ImGui::BeginChild("activity-list",{0,0},ImGuiChildFlags_Borders);
    const float period=std::max(1000.f,ImGui::GetIO().DeltaTime*1000.f);
    bool createdNew=false;
    for(const auto& item:activityLog_.items()) {
        if(item.id<activityAfter_) continue;
        const bool showSource=item.source=="mcp"?activityShowMcp_:item.source=="job"?activityShowMcp_:activityShowUi_;
        const bool failed=!item.succeeded();
        if(!showSource || (failed && !activityShowFailed_)) continue;
        // 失败的查询永远可见；成功的查询默认静默（除非显式打开）。
        if(item.query && item.succeeded() && !activityShowQuiet_) continue;
        createdNew=true;
        ImGui::PushID(static_cast<int>(item.id));
        const ImVec4 color=activityStatusColor(item);
        ImGui::TextColored(color,"%s",activityStatusText(item));
        ImGui::SameLine();
        const std::string clock=item.utc.size()>=19?item.utc.substr(11,8):item.utc;
        const unsigned age=static_cast<unsigned>(std::min<double>(9999.,
            (static_cast<double>(activityNewestSeen_)-static_cast<double>(item.id))*period*.001));
        const std::string ageText=age?" · "+std::to_string(age)+"s前":"";
        ImGui::TextDisabled("%s · %s · %s · %s%s",clock.c_str(),activitySourceLabel(item.source),
            activityScopeLabel(item.scope),item.method.c_str(),ageText.c_str());
        if(item.repeats>1) { ImGui::SameLine(); ImGui::TextDisabled("连续 %u 次",item.repeats); }
        // 摘要常常是一条完整路径（`export_animation output=D:\...`）。Selectable 不会
        // 换行，240~340 px 的栏宽下会被右边缘裁掉；这里按可用宽度省略，完整文本由
        // tooltip 给出（详情面板里也仍然逐字可读）。
        const std::string summaryText="  "+item.summary;
        const float summaryWidth=ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x;
        const std::string shownSummary=elideUtf8ToWidth(summaryText,summaryWidth);
        if(ImGui::Selectable(shownSummary.c_str(),activityDetailOpen_ && activityLastId_==item.id)) {
            activityLastId_=item.id; activityDetailOpen_=true;
        }
        if(shownSummary!=summaryText && ImGui::IsItemHovered()) ImGui::SetTooltip("%s",item.summary.c_str());
        if(!item.error.empty() && item.error.find("日志")==std::string::npos)
            ImGui::TextWrapped("  %s",item.error.c_str());
        if(activityDetailOpen_ && activityLastId_==item.id) {
            if(!item.error.empty()) ImGui::TextWrapped("原因：%s",item.error.c_str());
            ImGui::TextDisabled("id %llu · utc %s · source %s · scope %s · method %s · status %s",
                (unsigned long long)item.id,item.utc.c_str(),item.source.c_str(),item.scope.c_str(),
                item.method.c_str(),item.status.c_str());
            if(!item.detail.is_null() && !item.detail.empty()) {
                const std::string detail=item.detail.dump();
                ImGui::TextWrapped("%s",detail.c_str());
            }
            ImGui::TextDisabled("详情仅供查看：撤销/重做请用底部「编辑历史」。");
        }
        ImGui::PopID();
    }
    if(!createdNew) ImGui::TextDisabled("（无匹配记录：可调整筛选，或等待界面/MCP 操作）");
    // 自动跟随：只有用户本来就贴在末尾时才滚到最新，往上翻不会被抢回。
    if(activityFollow_) {
        const float scrollY=ImGui::GetScrollY(),scrollMax=ImGui::GetScrollMaxY();
        activityFollowing_=scrollMax<=1.f || scrollY>=scrollMax-4.f;
        if(activityFollowing_ && activityLastId_!=activityNewestSeen_) ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
    // activityAfter_ 只丢掉被容量上限淘汰的旧记录；activityNewestSeen_ 用于自动
    // 跟随的「有没有新记录」判断，用户往上翻时不会被抢回末尾。
    activityNewestSeen_=activityLog_.lastId();
    activityAfter_=activityLog_.firstAvailableId();
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
    if(frame>=8 && frame<=12) { const auto p=screen(frame<10?3:4,5); io.AddMousePosEvent(p.x,p.y); }
    if(frame==8) { smokeCreated_=selected_; io.AddMouseButtonEvent(0,true); }
    if(frame==12) io.AddMouseButtonEvent(0,false);
    if(frame==21 || frame==22) { io.AddMousePosEvent(closeCancelCenter_.x,closeCancelCenter_.y); io.AddMouseButtonEvent(0,frame==21); }
    if(frame==27 || frame==28) { io.AddKeyEvent(ImGuiMod_Ctrl,frame==27); io.AddKeyEvent(ImGuiKey_Z,frame==27); }
    if(!smokeUiArmed_) return;
    // U2: real wheel zoom and middle-button pan on the canvas the host actually
    // laid out (lastCanvasStart_/lastCanvasSize_ were captured during draw).
    const ImVec2 canvasCenter{lastCanvasStart_.x+lastCanvasSize_.x*.5f,lastCanvasStart_.y+lastCanvasSize_.y*.5f};
    if(frame==112) { io.AddMousePosEvent(canvasCenter.x,canvasCenter.y); io.AddMouseWheelEvent(0,3); }
    if(frame==113) { io.AddMousePosEvent(canvasCenter.x,canvasCenter.y); io.AddMouseButtonEvent(2,true); }
    if(frame==114) io.AddMousePosEvent(canvasCenter.x+60,canvasCenter.y+30);
    if(frame==115) io.AddMousePosEvent(canvasCenter.x+120,canvasCenter.y+70);
    if(frame==116) { io.AddMousePosEvent(canvasCenter.x+120,canvasCenter.y+70); io.AddMouseButtonEvent(2,false); }
    // U3: open the real ColorEdit4 picker with a click, then hold and drag inside
    // the popup for three frames before releasing. The picker rectangle came from
    // the popup window itself on the frame it opened.
    if(frame==117 && smokeColorReady_) { io.AddMousePosEvent(smokeColorCenter_.x,smokeColorCenter_.y); io.AddMouseButtonEvent(0,true); }
    if(frame==118) { io.AddMousePosEvent(smokeColorCenter_.x,smokeColorCenter_.y); io.AddMouseButtonEvent(0,false); }
    auto pickerPoint=[&](float fractionX,float fractionY) {
        return ImVec2{smokePickerMin_.x+smokePickerSize_.x*fractionX,smokePickerMin_.y+smokePickerSize_.y*fractionY};
    };
    // A newly opened popup needs one layout frame to acquire its auto-sized
    // rectangle and final on-screen position before the press can target it.
    if(frame==120 && smokePickerReady_) { const auto p=pickerPoint(0.20f,0.25f); io.AddMousePosEvent(p.x,p.y); io.AddMouseButtonEvent(0,true); }
    if(frame==121 && smokePickerReady_) { const auto p=pickerPoint(0.35f,0.35f); io.AddMousePosEvent(p.x,p.y); }
    if(frame==122 && smokePickerReady_) { const auto p=pickerPoint(0.50f,0.50f); io.AddMousePosEvent(p.x,p.y); }
    if(frame==123 && smokePickerReady_) { const auto p=pickerPoint(0.35f,0.55f); io.AddMousePosEvent(p.x,p.y); }
    if(frame==124) { const auto p=pickerPoint(0.35f,0.55f); io.AddMousePosEvent(p.x,p.y); io.AddMouseButtonEvent(0,false); }
}
void Editor::smokeAfterFrame(unsigned frame) {
    if(frame==18 && (smokeCreated_.empty() || std::abs(active().object(smokeCreated_)["properties"]["x"].get<double>()-4)>0.001))
        throw std::runtime_error("Palette click / canvas drag input test failed");
    if(frame==18 && requestClose()) throw std::runtime_error("Unsaved close did not request confirmation");
    if(frame==25 && !smokeCloseCancelled_) throw std::runtime_error("Unsaved close cancel input failed");
    if(frame==26) dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","move"},{"id",smokeCreated_},{"dx",2},{"dy",0}}})}}}},"mcp");
    if(frame==29) {
        if(std::abs(active().object(smokeCreated_)["properties"]["x"].get<double>()-4)>0.001) throw std::runtime_error("Human Ctrl+Z did not undo MCP edit");
        const auto events=active().history()["entries"];
        if(events.back()["source"]!="human" || !events.back().contains("relatedEvent")) throw std::runtime_error("Shared command history lacks cross-source undo link");
        historyOpen_=true;
    }
    if(frame>=72 && frame<=104 && (frame-72)%8==0) smokeScenario((frame-72)/8);
    if(frame==110) smokeCheck();
    if(frame==110) {
        // U2/U3 setup: a fresh demo document with a colour-bearing object so the
        // inspector renders a live ColorEdit4, then freeze a clean project so the
        // real view input can be checked against get_project.dirty.
        dispatch({{"method","replace"},{"args",{{"document",Document::demo()}}}},"system");
        selected_="ball"; fitRequested_=true; historyOpen_=false; animationWorkspace_=false;
        smokeColorBaseline_=active().object(selected_)["properties"]["color"];
        smokeColorRevision_=active().revision();
        smokeColorHistory_=active().history()["lastId"].get<uint64_t>();
        dispatch({{"method","save_project"},{"args",{{"path","build/smoke-u2.physproj"},{"overwrite",true}}}});
        smokeUiArmed_=true; smokeColorReady_=false; smokePickerReady_=false;
    }
    if(frame==111) {
        if(dispatch({{"method","get_project"}}).value("dirty",true))
            throw std::runtime_error("Saving a clean project left it dirty");
        smokeViewBaseline_=dispatch({{"method","get_view_state"}})["view"];
    }
    if(frame==117) {
        const auto view=dispatch({{"method","get_view_state"}})["view"];
        if(!view.value("canvasReady",false)) throw std::runtime_error("Canvas view was not reported during the view-input test");
        if(view.value("zoom",0.0)==smokeViewBaseline_.value("zoom",0.0))
            throw std::runtime_error("Real canvas wheel input did not change the view zoom");
        if(view["origin"]==smokeViewBaseline_["origin"] && view["pan"]==smokeViewBaseline_["pan"])
            throw std::runtime_error("Real canvas middle-button drag did not change the view pan");
        if(dispatch({{"method","get_project"}}).value("dirty",true))
            throw std::runtime_error("View-only canvas input dirtied the project");
    }
    // The host increments its frame counter before smokeAfterFrame: input 124
    // (mouse release) has been processed when this callback receives 125.
    if(frame==125) {
        if(!smokePickerReady_) throw std::runtime_error("ColorEdit4 smoke did not open its picker popup");
        if(active().object(selected_)["properties"]["color"]==smokeColorBaseline_)
            throw std::runtime_error("Real ColorEdit4 picker drag did not change the colour");
        if(active().revision()!=smokeColorRevision_+1)
            throw std::runtime_error("ColorEdit4 gesture did not commit exactly one revision");
        if(active().history()["lastId"].get<uint64_t>()!=smokeColorHistory_+1)
            throw std::runtime_error("ColorEdit4 gesture wrote more than one history entry");
        attempt({{"method","undo"}});
        if(active().object(selected_)["properties"]["color"]!=smokeColorBaseline_)
            throw std::runtime_error("Undo did not restore the colour from before the gesture");
        smokeUiArmed_=false;
    }
}
void Editor::smokeCheck() {
    // 操作日志侧栏：真实界面/MCP 操作必须被记录，查询不能刷屏，失败必须可见。
    // 这些断言直接跑在真实 smoke 输入之后，因此它们验证的是接线而不是纯函数。
    {
        const Json before=activitySnapshot();
        const uint64_t historyBefore=active().history()["lastId"].get<uint64_t>();
        if(active().object("smoke-circle")["properties"]["x"]!=2) throw std::runtime_error("Editor history smoke check failed");
        if(!before["sidebar"].value("open",false)) throw std::runtime_error("The operation log sidebar is not enabled");
        if(!activityVisible_) throw std::runtime_error("The operation log sidebar was not laid out during the smoke run");
        if(activityCollapsedBar_) throw std::runtime_error("The operation log sidebar collapsed inside a wide window");
        // 真实点击与 MCP 编辑都必须出现。
        const auto recorded=[&](const Json& page,const char* method,const char* source) {
            for(const auto& entry:page["entries"])
                if(entry["method"]==method && entry["source"]==source) return true;
            return false;
        };
        if(!recorded(before,"apply","human")) throw std::runtime_error("A human palette/drag edit never reached the operation log");
        if(!recorded(before,"apply","mcp")) throw std::runtime_error("An MCP edit never reached the operation log");
        if(!recorded(before,"undo","human") || !recorded(before,"redo","human"))
            throw std::runtime_error("Undo/redo never reached the operation log");
        if(recorded(before,"get_scene","human") || recorded(before,"get_scene","mcp"))
            throw std::runtime_error("A successful read-only query polluted the operation log");
        // 被拒绝的请求必须可见，而且不改变文档。
        const uint64_t rejectedEvent=active().history()["lastId"].get<uint64_t>();
        const uint64_t rejectedRevision=active().revision();
        try { dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","delete"},{"id","missing-smoke"}}})}}}}, "mcp"); }
        catch(const std::exception&) {}
        const Json afterFailure=activityLog_.query(0,ActivityLog::kCapacity);
        bool failedVisible=false;
        for(const auto& entry:afterFailure["entries"])
            if(entry["status"]=="error" && entry["method"]=="apply") failedVisible=true;
        if(!failedVisible) throw std::runtime_error("A rejected operation was not visible in the operation log");
        // 旧 journal（active().history() / get_history）是**文档**事件日志，它也会把
        // 被拒绝的请求记成一条 status="rejected" 的事件——这是既有行为，不是缺陷。
        // 正确的断言是「拒绝被记成 rejected 且没有推进修订」，而不是「拒绝不写 journal」。
        const auto journal=active().history(rejectedEvent,1);
        if(journal["lastId"].get<uint64_t>()!=rejectedEvent+1 || journal["entries"].empty())
            throw std::runtime_error("A rejected operation was not journalled");
        const auto& newest=journal["entries"].back();
        if(newest.value("status",std::string())!="rejected")
            throw std::runtime_error("A rejected operation was journalled as a successful transaction");
        if(newest.value("afterRevision",uint64_t(0))!=rejectedRevision || active().revision()!=rejectedRevision)
            throw std::runtime_error("A rejected operation still advanced the document revision");
        // 只读查询（get_activity_log）绝不能改动日志本身。
        const size_t logSize=activityLog_.size();
        const auto page=dispatch({{"method","get_activity_log"},{"args",{{"after",0},{"limit",5}}}});
        if(activityLog_.size()!=logSize) throw std::runtime_error("Querying the log changed the log");
        if(page["count"].get<size_t>()!=5 || page["entries"].size()!=5)
            throw std::runtime_error("get_activity_log ignored its bounded limit");
        const auto lastEntry=page["entries"].back();
        if(lastEntry["id"].get<uint64_t>()!=page["nextAfter"].get<uint64_t>())
            throw std::runtime_error("get_activity_log's cursor does not match its last entry");
        if(!page.contains("sidebar") || !page["sidebar"].value("independentOfHistory",false))
            throw std::runtime_error("get_activity_log did not report sidebar state");
        // 这一段 smoke 检查只被允许往 journal 里加那一条 reject：文档修订不变。
        if(active().history()["lastId"].get<uint64_t>()!=historyBefore+1)
            throw std::runtime_error("Activity-log smoke checks changed the document history");
        if(active().revision()!=rejectedRevision)
            throw std::runtime_error("Activity-log smoke checks changed the document revision");
    }
    // 后台任务进度的落点：日志栏完整显示时画在栏内；日志栏收起（F9）或窄到只剩
    // 按钮条、乃至整栏让位时必须走固定浮层——否则进度和「取消任务」会像真实截图里
    // 那样被挤出屏幕底部（没有可见落点）。没有任务在跑时两块都不画。
    {
        const auto surface=[&](bool running,bool shown,bool bar){return std::string(jobPanelSurface(running,shown,bar));};
        if(surface(true,true,false)!="activity")
            throw std::runtime_error("Job progress must stay inside the log column when that column is fully shown");
        if(surface(true,true,true)!="overlay" || surface(true,false,false)!="overlay")
            throw std::runtime_error("Job progress has no visible surface once the log column collapses to its button bar");
        if(surface(false,true,false)!="none" || surface(false,false,false)!="none")
            throw std::runtime_error("Job progress must not be drawn while no background job runs");
    }
    if(active().object("smoke-circle")["properties"]["x"]!=2) throw std::runtime_error("Editor history smoke check failed");
    if(active().evaluate("ball",15.0/9.8).vy>1e-10) throw std::runtime_error("Editor motion smoke check failed");
    const Json text={{"type","text"},{"properties",{{"text","zoom probe"},{"size",20}}}};
    const float savedZoom=zoom_,savedReference=referenceZoom_;
    referenceZoom_=40;zoom_=40;const auto a=bounds(text,{0,0,0,0});
    zoom_=80;const auto b=bounds(text,{0,0,0,0});
    if(std::abs(a.second.x-b.second.x)>1e-4f || std::abs(a.first.y-b.first.y)>1e-4f)
        throw std::runtime_error("Uniform zoom changed text layout");
    referenceZoom_=37.5f;storeLayout();referenceZoom_=0;applyLayout();
    if(std::abs(referenceZoom_-37.5f)>1e-4f)throw std::runtime_error("Zoom baseline was not restored");
    zoom_=savedZoom;referenceZoom_=savedReference;
    // U3: one history entry per finished colour/step edit, and a concurrent
    // document change cancels the preview instead of overwriting it.
    selected_="smoke-circle";
    const uint64_t beforeColor=active().history()["lastId"].get<uint64_t>();
    beginColorEdit(); colorPreview_[0]=0.25f; colorPreview_[1]=0.5f; colorPreview_[2]=0.75f; commitColorEdit();
    if(active().history()["lastId"].get<uint64_t>()!=beforeColor+1) throw std::runtime_error("Colour edit did not commit exactly one transaction");
    if(active().object(selected_)["properties"]["color"][0]!=0.25) throw std::runtime_error("Colour preview was not committed");
    const auto committedColor=active().object(selected_)["properties"]["color"];
    const uint64_t beforeCancel=active().history()["lastId"].get<uint64_t>();
    beginColorEdit(); colorPreview_[0]=0.1f;
    dispatch({{"method","apply"},{"args",{{"commands",Json::array({{{"op","move"},{"id",selected_},{"dx",0.5},{"dy",0}}})}}}},"mcp");
    commitColorEdit();
    if(active().object(selected_)["properties"]["color"]!=committedColor) throw std::runtime_error("Conflicting colour edit overwrote a concurrent MCP change");
    if(active().history()["lastId"].get<uint64_t>()!=beforeCancel+1) throw std::runtime_error("Cancelled colour edit still wrote history");
    const uint64_t beforeStep=active().history()["lastId"].get<uint64_t>();
    beginStepEdit(); stepPreview_=1; commitStepEdit();
    if(active().history()["lastId"].get<uint64_t>()!=beforeStep+1) throw std::runtime_error("Step edit did not commit exactly one transaction");
    if(active().object(selected_)["step"].get<int>()!=1) throw std::runtime_error("Step preview was not committed");
    // R3/scope cache: loading a document into a scope must drop that scope's
    // recorded "current" export camera before the next frame records a fresh one.
    if(!recordedView("scene").ready) throw std::runtime_error("Smoke precondition: no recorded scene view to invalidate");
    dispatch({{"method","load"},{"scope","scene"},{"args",{{"path","scenes/incline-arc.json"}}}},"system");
    if(recordedView("scene").ready) throw std::runtime_error("Loading a scene kept a stale recorded export view");
    // R3: the export glyph set and raster size must cover every exported frame,
    // not a coarse uniform sweep. 3 s at 10 fps renders frames 0..29, so the
    // 0.3 s frame is sampled; the old 16-step sweep (3*i/16) never hits it and
    // would bake ~14 px instead of the true 50 px peak. Component parameter text
    // ("R = {R} m") must be part of the same glyph set.
    {
        Json segments=Json::array();
        segments.push_back({{"start",0},{"end",3},{"expression","10+40*exp(-400*(t-0.3)*(t-0.3))"}});
        Json sizeChannel={{"kind","formula"},{"segments",segments}};
        Json probeAnimation={{"channels",{{"size",sizeChannel}}}};
        Json probeProperties={{"x",2},{"y",2},{"text","EXPORT SIZE PROBE"},{"size",10}};
        Json probeCommand={{"op","create"},{"id","export-size-probe"},{"type","text"},{"name","导出字号探针"},
            {"properties",probeProperties},{"animation",probeAnimation}};
        dispatch({{"method","apply"},{"scope","scene"},{"args",{{"commands",Json::array({probeCommand})}}}},"system");
        graphics::Options exportOptions; exportOptions.exportFrames=30;
        exportSnapshot_={{"fps",10}};  // exactly what jobRequest freezes for the export
        configureExportFonts(exportOptions);
        if(std::abs(exportOptions.exportFontPixelSize-50.f)>0.5f)
            throw std::runtime_error("configureExportFonts did not use the actual exported frames for the text size peak");
        if(exportOptions.exportGlyphText.find("EXPORT SIZE PROBE")==std::string::npos)
            throw std::runtime_error("configureExportFonts dropped an actual text object");
        if(exportOptions.exportGlyphText.find("R = 0.3 m")==std::string::npos)
            throw std::runtime_error("configureExportFonts dropped component parameter text");
    }
    // A1: the presented canvas viewport is reported, not inferred from the current values.
    const auto view=dispatch({{"method","get_view_state"}});
    if(!view.value("canvasReady",false) || view.value("canvasWidth",0.0)<=0 || !view.contains("view")
        || view["view"].value("source",std::string())!="live" || !view.contains("textWarnings"))
        throw std::runtime_error("get_view_state did not report the presented canvas viewport");
    // A2/A3: export view modes are explicit; current without a recorded view
    // must point at fit, fit needs explicit even dimensions.
    invalidateRecordedViews("scene");
    auto exportRejected=[&](const Json& args,const char* needle) {
        try { dispatch({{"method","export_animation"},{"expectedRevision",active().revision()},{"args",args}},"mcp"); }
        catch(const std::exception& e) { return std::string(e.what()).find(needle)!=std::string::npos; }
        return false;
    };
    if(!exportRejected({{"output","build/smoke-should-not-start.mp4"}},"fit"))
        throw std::runtime_error("current export without a recorded view did not suggest fit");
    if(!exportRejected({{"output","build/smoke-should-not-start.mp4"},{"view_mode","fit"},{"width",0},{"height",0}},"explicit"))
        throw std::runtime_error("fit export accepted an implicit canvas size");
    if(!exportRejected({{"output","build/smoke-should-not-start.mp4"},{"width",321},{"height",200}},"even"))
        throw std::runtime_error("export accepted an odd viewport size");
    if(!exportRejected({{"output","build/smoke-should-not-start.mp4"},{"width",0},{"height",720}},"paired"))
        throw std::runtime_error("export accepted a half-specified viewport size");
}
void Editor::captureFrame(unsigned frame,unsigned count) {
    active().dispatch({{"method","playback"},{"args",{{"time",active().scene()["duration"].get<double>()*(double(frame)/(count-1))},{"playing",false},{"step",3}}}},"preview");
}
}
