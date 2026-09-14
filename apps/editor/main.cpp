#include "editor.hpp"
#include "pipe.hpp"
#include "runtime_paths.hpp"
#include "stdio_transport.hpp"
#include "vulkan_host.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>

int wmain(int argc,wchar_t** argv) {
    auto utf8=[](const wchar_t* text) {
        const int length=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
        if(length<=0) throw std::runtime_error("Cannot decode command-line argument");
        std::string result(static_cast<size_t>(length),'\0');
        WideCharToMultiByte(CP_UTF8,0,text,-1,result.data(),length,nullptr,nullptr);
        result.pop_back(); return result;
    };
    graphics::Options options; options.title=L"物理示意编辑器 / Vulkan + C++";
    std::string pipe="physics-editor";
    std::string scene,project,exportJob,dataDir;
    bool componentSmoke=false,animationSmoke=false,stdioMode=false;
    for(int i=1;i<argc;++i) {
        const std::string arg=utf8(argv[i]);
        if(arg=="--smoke-test") { options.smoke=true; options.validation=true; pipe="physics-editor-smoke"; }
        else if(arg=="--component-smoke") { options.smoke=true; options.validation=true; componentSmoke=true; pipe="physics-editor-component-smoke"; scene="scenes/incline-arc.json"; }
        else if(arg=="--animation-smoke") { options.smoke=true;options.validation=true;animationSmoke=true;pipe="physics-editor-animation-smoke"; }
        else if(arg=="--project" && i+1<argc)project=utf8(argv[++i]);
        else if(arg=="--export-job" && i+1<argc)exportJob=utf8(argv[++i]);
        else if(arg=="--validation") options.validation=true;
        else if(arg=="--capture-animation") { options.capture=true; pipe="physics-editor-capture"; }
        else if(arg=="--pipe" && i+1<argc) pipe=utf8(argv[++i]);
        else if(arg=="--scene" && i+1<argc) scene=utf8(argv[++i]);
        else if(arg=="--stdio") stdioMode=true;
        else if(arg=="--data-dir" && i+1<argc) dataDir=utf8(argv[++i]);
        else if(arg=="--help") { std::cout<<"Physics editor [--validation] [--pipe NAME] [--scene FILE] [--smoke-test | --capture-animation | --stdio] [--data-dir DIR]\n"
            "  --stdio     explicit JSON-lines transport over stdin/stdout (editor side of the MCP bridge; not an MCP server)\n"
            "  --data-dir  explicit writable data directory; never silently replaced by a fallback\n"; return 0; }
        else { std::cerr<<"Unknown argument: "<<arg; return 1; }
    }
    try {
        if(stdioMode && (options.smoke || options.capture || !exportJob.empty()))
            throw std::runtime_error("--stdio is an explicit transport and cannot be combined with smoke/capture/export modes");
        if(stdioMode) {
            // Protocol purity: the transport writes to the stdout HANDLE directly,
            // so redirecting the C++ stdout stream to stderr keeps host diagnostics
            // out of the protocol even before the host itself is switched to stderr.
            if(!pipe.empty() && pipe!="physics-editor")
                std::cerr<<"[stdio] --pipe is ignored because --stdio was selected explicitly\n";
            std::cout.rdbuf(std::cerr.rdbuf());
        } else if(pipe.empty() || pipe.size()>100 || pipe.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos)
            throw std::runtime_error("Pipe name must contain 1..100 ASCII letters, digits, hyphens or underscores");
        physics_editor::Editor editor;
        if(!exportJob.empty()){
            using physics_editor::Json;
            const auto requestPath=std::filesystem::u8path(exportJob);
            if(std::filesystem::file_size(requestPath)>65536)throw std::runtime_error("Export request too large");
            Json request;{std::ifstream file(requestPath);file>>request;}
            const auto snapshotPath=std::filesystem::u8path(request.at("snapshot").get<std::string>());
            if(std::filesystem::file_size(snapshotPath)>64*1024*1024)throw std::runtime_error("Export snapshot too large");
            Json snapshot;{std::ifstream file(snapshotPath);file>>snapshot;}
            const double fps=request.at("fps");if(fps<1 || fps>60)throw std::runtime_error("Invalid export FPS");
            options.exportWidth=request.at("width");options.exportHeight=request.at("height");options.exportFrames=request.at("frames");
            options.exportDirectory=request.at("framesDirectory");
            options.validation=options.validation || request.value("validation",false);
            editor.setupExport(snapshot);
            editor.configureExportFonts(options);
            graphics::View view;view.draw=[&](float){editor.drawExport();};
            view.captureFrame=[&](unsigned frame,unsigned){editor.exportFrame(frame,fps);};
            return graphics::run(std::move(view),std::move(options));
        }
        const auto paths=physics_editor::initializeRuntimePaths(
            dataDir.empty()?std::nullopt:std::optional<std::filesystem::path>(std::filesystem::u8path(dataDir)));
        std::cerr<<"[runtime] data="<<paths.dataRoot.u8string()
                 <<" resources="<<(paths.resourcesFound?paths.resourceRoot.u8string():std::string("<not found>"))
                 <<(paths.dataFromFallback?" (fallback: the project directory is not writable)":"")<<'\n';
        editor.startJournal((paths.dataRoot/"history").u8string());
        if(!scene.empty()) editor.dispatch({{"method","load"},{"args",{{"path",scene}}}},"system");
        if(!project.empty())editor.dispatch({{"method","load_project"},{"args",{{"path",project}}}},"system");
        std::unique_ptr<physics_editor::Pipe> service;
        std::unique_ptr<physics_editor::StdioTransport> stdio;
        if(stdioMode) stdio=std::make_unique<physics_editor::StdioTransport>();
        else service=std::make_unique<physics_editor::Pipe>(std::wstring(pipe.begin(),pipe.end()));
        bool stdioExit=false,stdioClosedHandled=false;
        graphics::View view;
        view.draw=[&](float dt){editor.draw(dt);};
        view.closeRequested=[&](){return editor.requestClose();};
        view.shouldClose=[&](){return editor.shouldClose()||stdioExit;};
        view.poll=[&](){
            editor.pollJobs();
            if(stdio){
                const auto dispatch=[&](const physics_editor::Json& r){return editor.dispatch(r,"mcp");};
                stdio->poll(dispatch);
                if(!stdioClosedHandled && stdio->closed()){
                    stdioClosedHandled=true;
                    // Drain items queued before the terminal marker. After a transport
                    // failure we must not run more edits that can never be answered.
                    if(!stdio->failed())
                        for(int i=0;i<64 && stdio->hasPending();++i)
                            stdio->poll(dispatch,physics_editor::StdioTransport::kMaxQueuedRequests);
                    stdioExit=editor.requestClose();
                    std::cerr<<"[stdio] "<<stdio->closedReason()<<"; "
                             <<(stdioExit?"closing the editor":"unsaved changes; the window stays open for a manual save")<<'\n';
                }
            } else service->poll([&](const physics_editor::Json& r){return editor.dispatch(r,"mcp");});
        };
        view.takeShowRequest=[&](){return editor.takeShowRequest();};
        view.windowState=[&](bool visible,bool minimized,unsigned frames){editor.windowState(visible,minimized,frames);};
        view.presented=[&](){editor.presented();};
        view.captureFrame=[&](unsigned f,unsigned n){editor.captureFrame(f,n);};
        view.smokeInput=[&](unsigned frame){if(animationSmoke)editor.animationSmokeInput(frame); else if(componentSmoke) editor.componentSmokeInput(frame); else editor.smokeInput(frame);};
        view.smokeAfterFrame=[&](unsigned frame){if(animationSmoke)editor.animationSmokeAfterFrame(frame); else if(componentSmoke) editor.componentSmokeAfterFrame(frame); else editor.smokeAfterFrame(frame);};
        return graphics::run(std::move(view),std::move(options));
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
