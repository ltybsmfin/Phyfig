#include "editor.hpp"
#include "formula.hpp"
#include "formula_plan.hpp"
#include "formula_resources.hpp"
#include "animation.hpp"
#include "component.hpp"
#include "runtime_paths.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
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
// --- live background-job progress ------------------------------------------
// The worker publishes <job folder>/progress.json (stage, frames, elapsed,
// fps, estimate, overallFraction); BackgroundJob throttles reading it into
// state["progress"]. The panel below is drawn on every frame, so it stays
// visible while the export dialog is closed and for MCP-triggered jobs too.
const char* progressStageLabel(const std::string& stage){
    if(stage=="prepare")return "准备";
    if(stage=="render")return "渲染";
    if(stage=="encode")return "编码";
    if(stage=="commit")return "提交";
    if(stage=="done")return "完成";
    if(stage=="failed")return "失败";
    if(stage=="cancelled")return "已取消";
    return "进行中";
}
const char* progressPhaseLabel(const std::string& phase){
    if(phase=="palettegen")return "调色板";
    if(phase=="encode")return "视频流";
    if(phase=="render")return "渲染";
    return "";
}
std::string formatSeconds(double seconds){
    char buffer[64]{};
    if(seconds<60.)std::snprintf(buffer,sizeof(buffer),"%.1f 秒",seconds);
    else if(seconds<3600.)std::snprintf(buffer,sizeof(buffer),"%d 分 %02d 秒",static_cast<int>(seconds/60.),static_cast<int>(std::fmod(seconds,60.)));
    else std::snprintf(buffer,sizeof(buffer),"%d 时 %02d 分",static_cast<int>(seconds/3600.),static_cast<int>(std::fmod(seconds/60.,60.)));
    return buffer;
}
// Typed accessors: the panel must never throw on a field a worker published with
// an unexpected type, because it draws on the UI thread.
double jobNumber(const Json& object,const char* key,double fallback){
    return object.contains(key) && object[key].is_number()?object[key].get<double>():fallback;
}
bool jobHasNumber(const Json& object,const char* key){
    return object.contains(key) && object[key].is_number();
}
std::string jobText(const Json& object,const char* key){
    return object.contains(key) && object[key].is_string()?object[key].get<std::string>():std::string();
}
unsigned long long jobCount(const Json& object,const char* key){
    return object.contains(key) && object[key].is_number_unsigned()?object[key].get<unsigned long long>():0ULL;
}
// Progress bar plus stage, frames, elapsed/estimate and throughput. The bar only
// reaches 100% when the worker published stage "done": a fully rendered export
// that is still encoding or committing stays below 100% on purpose.
//
// 版式约定：日志栏只有 240~340 px 宽，**每一项都必须单独成行并按栏宽换行**。
// 以前这里用 SameLine 横向排开「预计剩余 / 帧率 / 渲染耗时」，在真实截图里被栏右
// 边缘整段裁掉，只剩半截数字。
void jobProgressUi(const BackgroundJob& job){
    if(!job.state.contains("progress") || !job.state["progress"].is_object()){
        ImGui::TextDisabled("等待渲染器发布进度…（进度发布失败不影响导出）");return;
    }
    const auto& progress=job.state["progress"];
    const std::string stage=jobText(progress,"stage"),phase=jobText(progress,"phase");
    const std::string message=jobText(progress,"message");
    const double overall=std::clamp(jobNumber(progress,"overallFraction",0.),0.,1.);
    const unsigned long long completed=jobCount(progress,"completedFrames"),total=jobCount(progress,"totalFrames");
    // 完成/失败/取消之后 worker 可能还在收尾，进程仍然 running。此时 fps 与剩余时间
    // 都是过期样本：界面绝不能一边说「已完成/已取消/失败」一边继续跳实时帧率。
    const bool finished=stage=="done",stopped=stage=="failed"||stage=="cancelled";
    const bool live=!finished && !stopped;
    char overlay[160]{};
    if(total>0) std::snprintf(overlay,sizeof(overlay),"%s %llu/%llu · %.1f%%",progressStageLabel(stage),completed,total,overall*100.);
    else std::snprintf(overlay,sizeof(overlay),"%s · %.1f%%",progressStageLabel(stage),overall*100.);
    const ImVec4 barColor=finished?ImVec4{.30f,.72f,.45f,1}
                          :stopped?ImVec4{1.f,.50f,.30f,1}
                          :ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,barColor);
    ImGui::ProgressBar(static_cast<float>(overall),ImVec2(-1,0),overlay);
    ImGui::PopStyleColor();
    std::string label=progressStageLabel(stage);
    if(!phase.empty() && phase!=stage && progressPhaseLabel(phase)[0])
        label+="（"+std::string(progressPhaseLabel(phase))+"）";
    ImGui::TextWrapped("阶段 %s · 已用 %s",label.c_str(),formatSeconds(jobNumber(progress,"elapsedSeconds",0.)).c_str());
    if(!live) {
        ImGui::TextWrapped(finished?"实时帧率与预计剩余已停止更新：本阶段已结束"
                                   :"实时帧率与预计剩余已停止更新：任务已结束");
    } else {
        // 「本阶段预计剩余」只说当前这一段：整个导出还要加上后面的编码与提交，
        // 不能让它读起来像整个任务的剩余时间。
        if(jobHasNumber(progress,"estimatedRemainingSeconds"))
            ImGui::TextWrapped("本阶段预计剩余 %s（不含之后的编码/提交）",
                formatSeconds(jobNumber(progress,"estimatedRemainingSeconds",0.)).c_str());
        else ImGui::TextWrapped("本阶段预计剩余：样本不足，暂时无法估计");
        if(jobHasNumber(progress,"fps")) ImGui::TextWrapped("渲染吞吐 %.1f 帧/秒",jobNumber(progress,"fps",0.));
        if(jobHasNumber(progress,"renderSeconds"))
            ImGui::TextWrapped("渲染阶段已耗时 %s",formatSeconds(jobNumber(progress,"renderSeconds",0.)).c_str());
    }
    // 失败/取消的原因必须能读完：这是用户唯一能看到 worker 原话的地方。
    if(!live && !message.empty()) ImGui::TextWrapped("原因：%s",message.c_str());
    // Rendering can be complete long before the export is: say so explicitly
    // instead of letting a full frame bar look like a finished export.
    if(total>0 && completed>=total && live && stage!="commit") {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.f,.75f,.2f,1));
        ImGui::TextWrapped("渲染帧已全部完成，导出尚未提交（后面还有编码/提交），请不要关闭编辑器");
        ImGui::PopStyleColor();
    }
}
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
    // 导出没有超时：一个 2K 长片的渲染时间是分钟级的，1800 s 会把正常的长导出判死
    // （BackgroundJob 约定 timeoutSeconds == 0 表示不设超时，取消按钮仍然有效）。
    exportJob_.start({executablePath(python),script.wstring(),L"--input",(folder/L"request.json").wstring(),L"--output",(folder/L"result.json").wstring()},folder,0);
    // 终态记录必须保留请求的发起方与作用范围：它们不在 BackgroundJob::state 里，
    // 而 state 只有 id/尺寸等运行信息。
    exportSource_=source;exportScope_=scope;
    exportJob_.state["id"]=folder.filename().u8string();exportJob_.state["frames"]=frames;exportJob_.state["output"]=input["output"];
    exportJob_.state["width"]=width;exportJob_.state["height"]=height;exportJob_.state["viewMode"]=viewMode;
    exportJob_.state["viewport"]={{"mode",viewMode},{"scope",scope},{"canvasWidth",view.width},{"canvasHeight",view.height},
        {"zoom",view.zoom},{"referenceZoom",view.referenceZoom>0?view.referenceZoom:view.zoom},
        {"origin",{view.origin.x,view.origin.y}},{"pan",{view.pan.x,view.pan.y}}};
    return exportJob_.state;
}
void Editor::reportJobOutcome(const char* kind,BackgroundJob& job,const std::string& source,
                              const std::string& scope,std::string& reported) {
    const std::string status=job.state.value("status",std::string());
    // 只有真正的终态才算结束：idle/running 都不是，而且 cancel() 之后 running() 已经
    // 变成 false，所以这里不能只看 poll() 的返回值。
    if(job.running() || !ActivityLog::isTerminalJobStatus(status)) return;
    const std::string key=ActivityLog::jobReportKey(job.state.value("id",std::string()),status);
    if(key==reported) return;   // 同一次运行只上报一次（state["id"] 每次 start() 都会换新）
    reported=key;
    const std::string error=job.state.contains("error") && job.state["error"].is_string()
        ?job.state["error"].get<std::string>():std::string();
    JobOutcome outcome;
    outcome.kind=kind;
    outcome.scope=scope.empty()?std::string("-"):scope;
    outcome.source=source.empty()?std::string("job"):source;
    outcome.status=status;
    outcome.error=error;
    outcome.jobId=job.state.value("id",std::string());
    // 请求本身已经由 dispatch() 记录过一次；这里只追加**独立**的终态记录
    // （startedId==0），既不回填也不重复记录请求。
    outcome.startedId=0;
    if(status=="completed") outcome.summary=std::string(kind)+" 任务完成";
    else if(status=="cancelled") outcome.summary=std::string(kind)+" 任务已取消";
    else outcome.summary=std::string(kind)+" 任务失败";
    recordActivityJob(outcome);
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
    // 异步任务的终态记录（完成/失败/取消）。取消会让 running() 立刻变成 false，
    // poll() 因此不会再返回 true，所以这里每帧无条件检查终态；去重靠每次 start()
    // 都会更换的 state["id"]。记录**独立追加**，不改写发起时那条请求记录，也不会
    // 把请求再记一遍。
    reportJobOutcome("formula",formulaJob_,formulaSource_,formulaScope_,formulaJobReported_);
    reportJobOutcome("export",exportJob_,exportSource_,exportScope_,exportJobReported_);
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
// 后台任务正文：日志栏内与兜底浮层共用同一份绘制，两处显示必然一致。
void Editor::jobsPanel(){
    bool first=true;
    for(auto entry:{std::pair<const char*,BackgroundJob*>{"formula",&formulaJob_},{"export",&exportJob_}}){
        BackgroundJob& job=*entry.second;
        if(!job.running())continue;
        if(!first)ImGui::Separator();
        first=false;
        const bool isExport=std::string(entry.first)=="export";
        const char* title=isExport?"正在导出动画…":"正在编译公式…";
        ImGui::TextUnformatted(title);
        // 取消任务是它唯一的出口：栏太窄放不下「标题 + 按钮」时换行，绝不横向裁掉。
        const float buttonWidth=ImGui::CalcTextSize("取消任务").x+ImGui::GetStyle().FramePadding.x*2.f;
        if(ImGui::CalcTextSize(title).x+ImGui::GetStyle().ItemSpacing.x+buttonWidth<=ImGui::GetContentRegionAvail().x)
            ImGui::SameLine();
        ImGui::PushID(entry.first);
        if(ImGui::Button("取消任务")){
            const std::string id=job.state.value("id",std::string());
            attempt({{"method","cancel_job"},{"args",{{"kind",entry.first},{"id",id}}}});
            // The last published progress is kept and said out loud here as well;
            // pollJobs() additionally appends the terminal record on the next frame.
            const bool cancelled=job.state.value("status",std::string())=="cancelled";
            const std::string failure=job.state.contains("error") && job.state["error"].is_string()
                ?job.state["error"].get<std::string>():std::string("取消失败");
            if(cancelled)status_=isExport?"导出已取消 · 保留最后进度，未提交的目标不会被修改":"公式编译已取消";
            else status_=failure;
        }
        ImGui::PopID();
        // A job that publishes no progress (the formula compiler) shows only the
        // line above, exactly as before.
        if(isExport || job.state.contains("progress"))jobProgressUi(job);
    }
}
// 日志栏被收起（F9）、只剩按钮条或整栏让位时的兜底进度浮层。
// 以前这里是在主布局流的末尾直接调 jobsPanel()：画布高度已经吃满剩余空间，那几行
// 会被状态行和播放控件挤出窗口底部（真实截图里进度就这样丢了）。浮层固定锚在画布列
// 左上角，任何窗口尺寸下都真的在屏幕内，进度与「取消任务」始终可见可点。
//
// 两个理由决定了它是**主窗口的子窗口**而不是独立顶层窗口：
//   * z 序：ImGui 每帧结束会把子窗口紧跟父窗口之后排序，所以子窗口永远画在主窗口
//     内容之上；而独立顶层窗口一旦主窗口被重新聚焦（点一下画布就会）就会被提到最前，
//     把浮层整个盖掉。子窗口同时仍排在导出对话框这类弹出窗口之下，不会挡住模态框。
//   * 不「预留高度」：画布高度参与文字/公式的像素基准拟合（fitCanvas 用 size.y 求
//     缩放），也参与已记录的导出取景；为一个只在任务运行期间出现的面板改动画布尺寸，
//     会让同一份文档因为「有没有任务在跑」得出不同的 referenceZoom / 导出像素。
//     子窗口按屏幕坐标定位、不参与主布局的列宽与行高计算，因此基准与离屏绘制不变。
void Editor::jobsFallbackOverlay(ImVec2 anchor,float width){
    if(!anyJobRunning()) return;
    const ImGuiViewport* viewport=ImGui::GetMainViewport();
    const float available=std::max(120.f,viewport->WorkSize.x-24.f);
    const float overlayWidth=std::clamp(width-16.f,120.f,std::min(520.f,available));
    const float minX=viewport->WorkPos.x+8.f;
    const float maxX=std::max(minX,viewport->WorkPos.x+viewport->WorkSize.x-8.f-overlayWidth);
    const float x=std::clamp(anchor.x+8.f,minX,maxX);
    const float y=std::max(anchor.y+8.f,viewport->WorkPos.y+8.f);
    const float titleWidth=ImGui::CalcTextSize("后台任务进度").x;
    const float buttonWidth=ImGui::CalcTextSize("展开日志").x+ImGui::GetStyle().FramePadding.x*2.f;
    const ImVec2 savedCursor=ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4{.17f,.18f,.21f,.98f});
    ImGui::SetCursorScreenPos({x,y});
    ImGui::BeginChild("##jobs-progress-overlay",{overlayWidth,0.f},
        ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY);
    // ImGui may put a child decoration into the parent's draw list. That
    // background would be behind the sibling canvas. Draw the opaque surface
    // in this child's own list so its text stays readable over the diagram.
    const ImVec2 panelPos=ImGui::GetWindowPos(),panelSize=ImGui::GetWindowSize();
    auto* panelDraw=ImGui::GetWindowDrawList();
    panelDraw->PushClipRect(panelPos,{panelPos.x+panelSize.x,panelPos.y+panelSize.y},false);
    panelDraw->AddRectFilled(panelPos,{panelPos.x+panelSize.x,panelPos.y+panelSize.y},IM_COL32(43,46,54,255));
    panelDraw->PopClipRect();
    ImGui::TextUnformatted("后台任务进度");
    if(titleWidth+ImGui::GetStyle().ItemSpacing.x+buttonWidth<=ImGui::GetContentRegionAvail().x) ImGui::SameLine();
    if(ImGui::SmallButton("展开日志")) activityOpen_=true;
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("重新展开右侧操作日志栏（F9）");
    ImGui::TextWrapped("操作日志栏已收起，进度固定显示在这里");
    jobsPanel();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(savedCursor);
}
bool Editor::anyJobRunning() const { return formulaJob_.running()||exportJob_.running(); }
// 只读诊断快照：进度面板画在哪里、有没有任务在跑、发布了什么阶段。它只报告已经
// 画出来（或将要画）的状态，从不改动任务本身。
Json Editor::jobSnapshot() const {
    auto describe=[](const BackgroundJob& job){
        Json out={{"running",job.running()},{"id",job.state.value("id",std::string())},
                  {"status",job.state.value("status",std::string())},{"progress",nullptr}};
        if(job.state.contains("progress") && job.state["progress"].is_object()){
            const auto& progress=job.state["progress"];
            out["progress"]={{"stage",jobText(progress,"stage")},
                {"completedFrames",jobCount(progress,"completedFrames")},
                {"totalFrames",jobCount(progress,"totalFrames")},
                {"overallFraction",jobNumber(progress,"overallFraction",0.)},
                {"estimatedRemainingSeconds",jobHasNumber(progress,"estimatedRemainingSeconds")
                    ?Json(jobNumber(progress,"estimatedRemainingSeconds",0.)):Json(nullptr)}};
        }
        return out;
    };
    return {{"formula",describe(formulaJob_)},{"export",describe(exportJob_)},
        {"surface",jobsSurface_},{"overlayVisible",jobsSurface_=="overlay"},
        {"panelVisible",jobsSurface_!="none"},{"running",anyJobRunning()}};
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
