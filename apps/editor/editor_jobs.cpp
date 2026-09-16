#include "editor.hpp"
#include "formula.hpp"
#include "formula_plan.hpp"
#include "formula_resources.hpp"
#include "animation.hpp"
#include "component.hpp"
#include "runtime_paths.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <cstring>

namespace physics_editor {
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
int evenCanvasDimension(float value,const char* label) {
    require(std::isfinite(value),"Recorded canvas size is not finite");
    const int rounded=static_cast<int>(std::lround(value));
    require(rounded<=4096,std::string("Recorded ")+label+" exceeds 4096; pass explicit even width/height instead");
    require(rounded>=16,std::string("Recorded ")+label+" is below 16 px; pass explicit even width/height instead");
    // Odd canvas sizes are rounded down by one pixel: MP4/GIF yuv420p needs even
    // dimensions, and silently rescaling to 4096 is never acceptable.
    return rounded%2==0?rounded:rounded-1;
}
std::filesystem::path jobDirectory(){
    static unsigned sequence=0;
    std::error_code error;
    // Scratch lives under the writable data root (journal/scratch/export work),
    // never under the process working directory: the editor may have been
    // started from an unrelated directory by the MCP server or a shortcut.
    const auto base=runtimePaths().dataRoot/std::filesystem::u8path("build")/std::filesystem::u8path("jobs");
    std::filesystem::create_directories(base,error);
    require(!error,std::string("Cannot create the background job directory ")+base.u8string()+": "+error.message());
    for(unsigned attempt=0;attempt<10000;++attempt){
        auto path=base/(std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())+"-"+std::to_string(++sequence));
        std::error_code createError;
        if(std::filesystem::create_directory(path,createError))return path;
        if(createError && createError!=std::errc::file_exists)
            throw std::runtime_error("Cannot create a background job directory under "+base.u8string()+": "+createError.message());
    }
    throw std::runtime_error("Cannot allocate a unique background job directory under "+base.u8string());
}
// Resources are read-only and ship with the source tree; they are resolved from
// the discovered resource root, not from the working directory. A missing pair
// is a hard, explicit error instead of a silent fallback.
std::filesystem::path resourceFile(const char* relative){
    const auto& paths=runtimePaths();
    if(!paths.resourcesFound || paths.resourceRoot.empty())
        throw std::runtime_error(std::string("Editor resources not found (expected ")+relative+
            "). Start the editor from the source tree, or keep the tools/ and assets/formulas/ directories beside the executable or one of its parent directories.");
    return paths.resourceRoot/std::filesystem::u8path(relative);
}
void writeJson(const std::filesystem::path& path,const Json& value){
    std::ofstream file(path,std::ios::binary);file<<value.dump();file.flush();require(bool(file),"Cannot write background task input");
}
std::wstring wide(const std::string& s){return std::filesystem::u8path(s).wstring();}
std::wstring executablePath(const std::string& name){
    const auto path=wide(name);wchar_t resolved[32768]{};
    const DWORD size=SearchPathW(nullptr,path.c_str(),L".exe",32768,resolved,nullptr);
    require(size>0 && size<32768,"Cannot find selected Python executable");return resolved;
}
bool exists(const Document& doc,const std::string& id){for(const auto& o:doc.scene()["objects"])if(o["id"]==id)return true;return false;}
}
void Editor::syncFormulaResources(){
    formula_resources::validate(workspace_.resources());
    require(workspace_.resources().at("formulaScale")==formula_cache::displayScale,"Unsupported formula scale");
    std::vector<math_text::Mask> masks;
    std::vector<formula_plan::Plan> plans;
    for(const auto& a:workspace_.resources()["formulaMasks"]){
        bool builtin=false;
        for(const auto& b:formula_cache::entries)if(a["name"]==b.name){
            require(a["width"]==b.width && a["height"]==b.height && a["rle"]==std::vector<unsigned char>(b.rle,b.rle+b.bytes),"Built-in formula id conflicts with project asset");builtin=true;
        }
        if(!builtin){
            masks.push_back({a["name"],a["width"],a["height"],a["rle"].get<std::vector<unsigned char>>()});
            // The plan travels with the asset; a formula without one keeps the
            // reading-order animation instead of guessing a structure.
            if(a.contains("writingPlan")){
                auto plan=formula_plan::parse(a["writingPlan"]);
                plan.name=a["name"].get<std::string>();
                plans.push_back(std::move(plan));
            }
        }
    }
    math_text::setCustom(std::move(masks));
    formula_plan::setInstalled(std::move(plans));
}
Json Editor::jobRequest(const Json& request,const std::string& source){
    const std::string method=request.at("method");const auto args=request.value("args",Json::object());
    if(method=="get_job" || method=="cancel_job"){
        const auto kind=args.value("kind",std::string("formula"));require(kind=="formula" || kind=="export","Job kind must be formula or export");
        auto& job=kind=="formula"?formulaJob_:exportJob_;
        if(method=="cancel_job"){
            require(args.contains("id") && job.state.value("id",std::string())==args["id"],"Job id changed; read get_job before cancelling");job.cancel();
        }
        return job.state;
    }
    const auto scope=request.value("scope",std::string("scene"));auto& doc=workspace_.document(scope);
    if(source=="mcp")require(request.contains("expectedRevision"),"Read scene revision before starting a job");
    if(request.contains("expectedRevision"))require(request["expectedRevision"]==doc.revision(),"Revision conflict; read scene again");
    require(!doc.previewing(),"Exit history preview before starting a job");
    const std::string python=args.value("python",std::string(pythonPath_));require(!python.empty() && python.size()<2048,"Invalid Python path");
    if(method=="set_formula"){
        require(!formulaJob_.running(),"A formula is still compiling");
        const auto latex=args.at("latex").get<std::string>(),id=args.at("id").get<std::string>();
        require(!latex.empty() && latex.size()<=4096,"LaTeX source must contain 1..4096 bytes");
        require(!id.empty() && id.size()<=128,"Invalid formula object id");
        if(exists(doc,id))require(doc.object(id)["type"]=="formula","Target object is not a formula");
        const auto script=resourceFile("tools/render_formula.py");
        const auto folder=jobDirectory();writeJson(folder/L"request.json",{{"latex",latex}});
        formulaJob_.start({executablePath(python),script.wstring(),L"--input",(folder/L"request.json").wstring(),L"--output",(folder/L"result.json").wstring()},folder,90);
        formulaRequest_=args;formulaSource_=source;formulaScope_=scope;formulaRevision_=doc.revision();formulaGeneration_=scope=="component"?componentGeneration_:workspaceGeneration_;
        formulaJob_.state["id"]=folder.filename().u8string();formulaJob_.state["scope"]=scope;
        formulaJob_.state["object"]=id;
        if(source=="mcp" && followMcp_){switchScope(scope);showRequested_=true;}
        return formulaJob_.state;
    }
    require(method=="export_animation","Unknown job operation");require(!exportJob_.running(),"An export is already running");
    const std::string viewMode=args.value("view_mode",args.value("viewMode",std::string("current")));
    require(viewMode=="current" || viewMode=="fit","view_mode must be current or fit");
    const int requestedWidth=args.value("width",0),requestedHeight=args.value("height",0);
    require((requestedWidth==0)==(requestedHeight==0),"width and height must be paired: both 0 to match the canvas, or both explicit");
    require(requestedWidth>=0 && requestedHeight>=0 && requestedWidth<=4096 && requestedHeight<=4096,
        "Export size must be even, from 16 to 4096");
    if(viewMode=="fit")
        require(requestedWidth>0,"fit requires explicit even width/height; width=height=0 only matches the current canvas");
    if(requestedWidth>0)
        require(requestedWidth%2==0 && requestedHeight%2==0 && requestedWidth>=16 && requestedHeight>=16,
            "Export size must be even, from 16 to 4096");
    const int fps=args.value("fps",24);
    require(fps>=1 && fps<=60,"Export FPS must be from 1 to 60");
    const double duration=doc.scene()["duration"];const unsigned frames=static_cast<unsigned>(std::ceil(duration*fps));
    require(frames>=1 && frames<=3600,"Export exceeds 3600 frames; reduce FPS or scene duration");
    const auto format=args.value("format",std::string("mp4"));require(format=="mp4" || format=="gif" || format=="png","Unknown export format");
    const std::string output=args.at("output");require(!output.empty() && output.size()<2048,"Invalid export path");
    int width=requestedWidth,height=requestedHeight;
    CanvasView view;
    if(viewMode=="current") {
        view=recordedView(scope);
        require(view.ready,"No recorded current view for scope '"+scope+"'; show that scope once or use view_mode='fit' with explicit even width/height");
        if(width==0) { width=evenCanvasDimension(view.physicalWidth,"canvas width"); height=evenCanvasDimension(view.physicalHeight,"canvas height"); }
    } else {
        // fit is an independent viewport for the requested size and the target
        // scope: it never reads the window/canvas viewport or the first frame.
        view=fitView(scope,static_cast<float>(width),static_cast<float>(height));
    }
    const auto script=resourceFile("tools/export_animation.py");
    const auto folder=jobDirectory();
    Json snapshot={{"scene",doc.snapshot()["document"]},{"resources",workspace_.resources()},{"step",doc.step()},
        {"viewMode",viewMode},{"scope",scope},
        {"zoom",view.zoom},{"referenceZoom",view.referenceZoom>0?view.referenceZoom:view.zoom},
        {"canvasWidth",view.width},{"canvasHeight",view.height},
        {"origin",{view.origin.x,view.origin.y}},{"pan",{view.pan.x,view.pan.y}},
        {"grid",view.grid},{"trails",view.trails},
        // The export child samples frame i at i/fps; the glyph set must cover
        // exactly those frames, so the requested rate travels with the snapshot.
        {"fps",fps}};
    writeJson(folder/L"snapshot.json",snapshot);
    wchar_t executable[32768]{};require(GetModuleFileNameW(nullptr,executable,32768)>0,"Cannot find renderer executable");
    Json input={{"executable",std::filesystem::path(executable).u8string()},{"snapshot",(folder/L"snapshot.json").u8string()},
        {"output",std::filesystem::absolute(std::filesystem::u8path(output)).u8string()},{"format",format},{"width",width},{"height",height},
        {"fps",fps},{"frames",frames},{"duration",duration},{"viewMode",viewMode},
        {"ffmpeg",args.value("ffmpeg",std::string(ffmpegPath_))},{"overwrite",args.value("overwrite",false)},{"validation",args.value("validation",false)}};
    writeJson(folder/L"request.json",input);
    exportJob_.start({executablePath(python),script.wstring(),L"--input",(folder/L"request.json").wstring(),L"--output",(folder/L"result.json").wstring()},folder,1800);
    exportJob_.state["id"]=folder.filename().u8string();exportJob_.state["frames"]=frames;exportJob_.state["output"]=input["output"];
    exportJob_.state["width"]=width;exportJob_.state["height"]=height;exportJob_.state["viewMode"]=viewMode;
    exportJob_.state["viewport"]={{"mode",viewMode},{"scope",scope},{"canvasWidth",view.width},{"canvasHeight",view.height},
        {"zoom",view.zoom},{"referenceZoom",view.referenceZoom>0?view.referenceZoom:view.zoom},
        {"origin",{view.origin.x,view.origin.y}},{"pan",{view.pan.x,view.pan.y}}};
    return exportJob_.state;
}
void Editor::pollJobs(){
    if(formulaJob_.poll()){
        if(formulaJob_.state["status"]=="completed"){
            const auto oldResources=workspace_.resources();
            try{
                require((formulaScope_=="component"?componentGeneration_:workspaceGeneration_)==formulaGeneration_,"Target document was replaced while formula compiled; submit again");
                auto& doc=workspace_.document(formulaScope_);
                require(doc.revision()==formulaRevision_,"Document changed while formula compiled; submit again");
                const auto result=formulaJob_.state.at("result");
                require(result.at("latex")==formulaRequest_["latex"] && result.at("displayScale")==.5,"Unexpected formula worker result");
                Json asset={{"name",result.at("name")},{"latex",result.at("latex")},{"width",result.at("width")},{"height",result.at("height")},{"rle",result.at("rle")}};
                if(result.contains("writingPlan"))asset["writingPlan"]=result["writingPlan"];
                Json resources=workspace_.resources();
                bool found=false;
                for(auto& a:resources["formulaMasks"])if(a["name"]==asset["name"]){
                    // Same id means the same LaTeX and the same bitmap; the
                    // optional plan is additive, so an asset stored by an older
                    // build adopts it instead of reporting a collision.
                    auto existing=a,generated=asset;
                    existing.erase("writingPlan");generated.erase("writingPlan");
                    if(!existing.contains("latex"))existing["latex"]=generated["latex"];
                    require(existing==generated,"Formula asset id collision");
                    if(!a.contains("latex"))a["latex"]=asset["latex"];
                    if(!a.contains("writingPlan") && asset.contains("writingPlan"))a["writingPlan"]=asset["writingPlan"];
                    found=true;
                }
                if(!found)resources["formulaMasks"].push_back(asset);
                workspace_.setResources(resources);
                formula_resources::validate(workspace_.resources());
                const std::string id=formulaRequest_["id"];
                Json command;
                if(exists(doc,id))command={{"op","update"},{"id",id},{"patch",{{"properties",{{"formula",asset["name"]}}}}}};
                else command={{"op","create"},{"type","formula"},{"id",id},{"name","LaTeX 公式"},{"properties",{{"formula",asset["name"]},{"x",formulaRequest_.value("x",3.)},{"y",formulaRequest_.value("y",5.)}}}};
                syncFormulaResources();
                dispatch({{"method","apply"},{"scope",formulaScope_},{"expectedRevision",formulaRevision_},{"args",{{"commands",Json::array({command})}}}},formulaSource_);
                if(scope_==formulaScope_)selected_=id;
                formulaJob_.state["result"].erase("rle");formulaJob_.state["revision"]=doc.revision();status_="公式已添加 · 可撤销，保存工程可保留源文本和渲染结果";
            }catch(const std::exception& e){workspace_.setResources(oldResources);syncFormulaResources();formulaJob_.state["status"]="failed";formulaJob_.state["error"]=e.what();}
        }
        if(formulaJob_.state["status"]=="failed")status_="公式失败："+formulaJob_.state.value("error",std::string());
        if(formulaJob_.state.contains("result"))formulaJob_.state["result"].erase("rle");
    }
    if(exportJob_.poll()){
        if(exportJob_.state.contains("result") && exportJob_.state["result"].contains("error") && exportJob_.state["result"]["error"].is_object())
            exportJob_.state["error"]=exportJob_.state["result"]["error"].value("message",std::string("Export failed"));
        // Publish the dimensions the worker actually encoded, not just the request.
        if(exportJob_.state["status"]=="completed" && exportJob_.state.contains("result") && exportJob_.state["result"].is_object())
            for(const char* key:{"width","height","frames","duration","format","output"})
                if(exportJob_.state["result"].contains(key)) exportJob_.state[key]=exportJob_.state["result"][key];
        status_=exportJob_.state["status"]=="completed"?"导出完成："+exportJob_.state.value("output",std::string()):"导出失败："+exportJob_.state.value("error",std::string());
    }
}
void Editor::formulaPanel(const Json& object){
    const auto editKey=scope_+":"+object["id"].get<std::string>()+":"+object["properties"]["formula"].get<std::string>();
    if(formulaObject_!=editKey){
        formulaObject_=editKey;latexInput_[0]=0;
        for(const auto& a:workspace_.resources()["formulaMasks"])if(a["name"]==object["properties"]["formula"] && a.contains("latex"))strcpy_s(latexInput_,a["latex"].get_ref<const std::string&>().c_str());
        if(!latexInput_[0]){
            try{
                const auto path=resourceFile("assets/formulas/formulas.json");
                std::ifstream file(path,std::ios::binary);require(bool(file),std::string("Cannot open ")+path.u8string());
                Json sources;file>>sources;require(!sources.is_discarded(),std::string("Cannot parse ")+path.u8string());
                const auto key=object["properties"]["formula"].get<std::string>();
                if(sources.contains(key))strcpy_s(latexInput_,sources[key].get_ref<const std::string&>().c_str());
            }catch(const std::exception& e){status_=std::string("公式源文本不可用：")+e.what();}
        }
    }
    if(ImGui::CollapsingHeader("LaTeX 源公式",ImGuiTreeNodeFlags_DefaultOpen)){
        ImGui::InputTextMultiline("##latex",latexInput_,sizeof(latexInput_),{-1,75});
        ImGui::InputText("Python",pythonPath_,sizeof(pythonPath_));
        ImGui::BeginDisabled(formulaJob_.running());
        if(ImGui::Button("编译并替换公式"))attempt({{"method","set_formula"},{"args",{{"latex",latexInput_},{"id",object["id"]},{"python",pythonPath_}}}});
        ImGui::EndDisabled();ImGui::TextWrapped("输入数学 LaTeX（不带外层 $），如 \\frac{1}{2}mv^2。需要 MiKTeX、Pillow、pypdfium2。");
    }
}
void Editor::jobsPanel(){
    for(auto entry:{std::pair<const char*,BackgroundJob*>{"formula",&formulaJob_},{"export",&exportJob_}})if(entry.second->running()){
        ImGui::TextUnformatted(entry.first==std::string("formula")?"正在编译公式…":"正在导出动画…");ImGui::SameLine();
        ImGui::PushID(entry.first);if(ImGui::SmallButton("取消任务"))attempt({{"method","cancel_job"},{"args",{{"kind",entry.first},{"id",entry.second->state["id"]}}}});ImGui::PopID();
    }
}
void Editor::exportDialog(){
    if(exportDialogOpen_){ImGui::OpenPopup("导出动画");exportDialogOpen_=false;}
    if(ImGui::BeginPopupModal("导出动画",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::TextUnformatted("导出目标范围冻结文档与解题步骤的完整运动过程；导出期间可以继续编辑。");
        ImGui::Combo("视图模式",&exportViewMode_,"当前构图 current\0自动适配 fit\0");
        const bool fit=exportViewMode_==1;
        const bool matchCanvas=exportMatchCanvas_ && !fit;
        const auto recorded=recordedView(scope_);
        ImGui::BeginDisabled(fit);
        ImGui::Checkbox("与画布一致（使用已记录画布尺寸）",&exportMatchCanvas_);
        ImGui::EndDisabled();
        if(matchCanvas) {
            if(recorded.ready) {
                auto evenOr=[](float value){const int rounded=static_cast<int>(std::lround(value));return (rounded<16||rounded>4096)?0:(rounded%2==0?rounded:rounded-1);};
                const int evenWidth=evenOr(recorded.physicalWidth),evenHeight=evenOr(recorded.physicalHeight);
                if(evenWidth>0 && evenHeight>0)
                    ImGui::Text("画布（物理像素）：%.0f×%.0f → 取偶 %d×%d",recorded.physicalWidth,recorded.physicalHeight,evenWidth,evenHeight);
                else ImGui::TextColored({1,.5f,.3f,1},"画布尺寸超出 16..4096；请改用自动适配并填写显式尺寸。");
            } else ImGui::TextColored({1,.5f,.3f,1},"当前范围没有已记录画布；请改用自动适配并填写显式尺寸。");
        }
        ImGui::BeginDisabled(matchCanvas);
        ImGui::InputInt("宽度",&exportWidth_);ImGui::InputInt("高度",&exportHeight_);
        ImGui::EndDisabled();
        ImGui::TextDisabled("显式尺寸必须为 16..4096 的偶数；与画布一致时按偶数向下取整，不会缩放到 4096。");
        ImGui::Combo("格式",&exportFormat_,"MP4\0GIF\0PNG 帧目录\0");
        ImGui::InputText("输出路径",exportPath_,sizeof(exportPath_));ImGui::InputInt("帧率",&exportFps_);
        ImGui::InputText("Python",pythonPath_,sizeof(pythonPath_));ImGui::InputText("ffmpeg",ffmpegPath_,sizeof(ffmpegPath_));
        ImGui::Checkbox("允许替换已有视频文件",&exportOverwrite_);
        ImGui::BeginDisabled(exportJob_.running());if(ImGui::Button("开始导出")){
            attempt({{"method","export_animation"},{"args",{{"output",exportPath_},{"format",exportFormat_==0?"mp4":exportFormat_==1?"gif":"png"},
                {"view_mode",fit?"fit":"current"},{"width",matchCanvas?0:exportWidth_},{"height",matchCanvas?0:exportHeight_},
                {"fps",exportFps_},{"python",pythonPath_},{"ffmpeg",ffmpegPath_},{"overwrite",exportOverwrite_}}}});
            if(exportJob_.running())ImGui::CloseCurrentPopup();
        }ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("关闭"))ImGui::CloseCurrentPopup();ImGui::TextWrapped("%s",status_.c_str());ImGui::EndPopup();
    }
}
void Editor::setupExport(const Json& snapshot){
    formula_resources::validate(snapshot.at("resources"));workspace_.setResources(snapshot["resources"]);
    if(snapshot.at("scene").contains("componentDefinition")){
        workspace_.dispatch({{"method","open_component"},{"args",{{"id",snapshot["scene"]["componentDefinition"]["id"]}}}},"system");scope_="component";
    }
    active().dispatch({{"method","replace"},{"args",{{"document",snapshot.at("scene")}}}},"system");
    syncFormulaResources();exportSnapshot_=snapshot;
}
void Editor::exportFrame(unsigned frame,double fps){
    active().dispatch({{"method","playback"},{"args",{{"time",std::min(active().scene()["duration"].get<double>(),frame/fps)},{"step",exportSnapshot_.at("step")},{"playing",false}}}},"preview");
}
void Editor::drawExport(){
    const auto size=ImGui::GetIO().DisplaySize;auto* draw=ImGui::GetBackgroundDrawList();
    const float w=exportSnapshot_.at("canvasWidth"),h=exportSnapshot_.at("canvasHeight");
    require(std::isfinite(w) && std::isfinite(h) && w>=1 && h>=1,"Invalid snapshot viewport");
    const float scale=std::min(size.x/w,size.y/h);zoom_=exportSnapshot_.at("zoom").get<float>()*scale;referenceZoom_=exportSnapshot_.at("referenceZoom");
    require(std::isfinite(zoom_) && zoom_>0 && referenceZoom_>=8 && referenceZoom_<=1200,"Invalid snapshot zoom");
    origin_={(size.x-w*scale)*.5f+exportSnapshot_["origin"][0].get<float>()*scale,(size.y-h*scale)*.5f+exportSnapshot_["origin"][1].get<float>()*scale};
    draw->AddRectFilled({0,0},size,IM_COL32(252,251,248,255));draw->PushClipRect({0,0},size,true);
    if(exportSnapshot_.value("grid",false)){
        for(float x=std::fmod(origin_.x,zoom_);x<size.x;x+=zoom_)draw->AddLine({x,0},{x,size.y},IM_COL32(228,231,233,255));
        for(float y=std::fmod(origin_.y,zoom_);y<size.y;y+=zoom_)draw->AddLine({0,y},{size.x,y},IM_COL32(228,231,233,255));
    }
    const bool component=active().scene().contains("componentDefinition");
    const auto objects=component?components::materialize(active().scene()):active().scene()["objects"];
    for(auto o:objects){
        if(!o["visible"].get<bool>() || o["step"].get<int>()>active().step())continue;
        o["properties"]=animation::properties(o,active().time(),active().scene()["duration"]);active().registry().get(o["type"]).validate(o["properties"]);
        const std::string id=o["id"];const auto state=component?State{o["properties"]["x"],o["properties"]["y"],0,0}:active().evaluate(id,active().time());
        if(exportSnapshot_.value("trails",false) && o.contains("motion")){
            auto previous=screen(active().evaluate(id,0).x,active().evaluate(id,0).y);
            for(int i=1;i<=100;++i){auto s=active().evaluate(id,active().time()*i/100.);auto next=screen(s.x,s.y);draw->AddLine(previous,next,IM_COL32(153,180,197,140),1.5f*canvasScale());previous=next;}
        }
        const auto& c=o["properties"]["color"];
        renderers_.at(o["type"].get<std::string>())(draw,o,state,ImGui::ColorConvertFloat4ToU32({c[0],c[1],c[2],c[3]}));
    }
    draw->PopClipRect();
}
}
