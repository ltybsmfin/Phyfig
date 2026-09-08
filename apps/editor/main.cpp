#include "editor.hpp"
#include "pipe.hpp"
#include "vulkan_host.hpp"
#include <filesystem>
#include <iostream>
#include <memory>

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
    std::string scene;
    bool componentSmoke=false;
    for(int i=1;i<argc;++i) {
        const std::string arg=utf8(argv[i]);
        if(arg=="--smoke-test") { options.smoke=true; options.validation=true; pipe="physics-editor-smoke"; }
        else if(arg=="--component-smoke") { options.smoke=true; options.validation=true; componentSmoke=true; pipe="physics-editor-component-smoke"; scene="scenes/incline-arc.json"; }
        else if(arg=="--validation") options.validation=true;
        else if(arg=="--capture-animation") { options.capture=true; pipe="physics-editor-capture"; }
        else if(arg=="--pipe" && i+1<argc) pipe=utf8(argv[++i]);
        else if(arg=="--scene" && i+1<argc) scene=utf8(argv[++i]);
        else if(arg=="--help") { std::cout<<"Physics editor [--validation] [--pipe NAME] [--scene FILE] [--smoke-test | --capture-animation]\n"; return 0; }
        else { std::cerr<<"Unknown argument: "<<arg; return 1; }
    }
    try {
        if(pipe.empty() || pipe.size()>100 || pipe.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=std::string::npos)
            throw std::runtime_error("Pipe name must contain 1..100 ASCII letters, digits, hyphens or underscores");
        physics_editor::Editor editor;
        editor.startJournal((std::filesystem::current_path()/"history").u8string());
        if(!scene.empty()) editor.dispatch({{"method","load"},{"args",{{"path",scene}}}},"system");
        physics_editor::Pipe service(std::wstring(pipe.begin(),pipe.end()));
        graphics::View view;
        view.draw=[&](float dt){editor.draw(dt);};
        view.closeRequested=[&](){return editor.requestClose();};
        view.shouldClose=[&](){return editor.shouldClose();};
        view.poll=[&](){service.poll([&](const physics_editor::Json& r){return editor.dispatch(r,"mcp");});};
        view.takeShowRequest=[&](){return editor.takeShowRequest();};
        view.windowState=[&](bool visible,bool minimized,unsigned frames){editor.windowState(visible,minimized,frames);};
        view.presented=[&](){editor.presented();};
        view.captureFrame=[&](unsigned f,unsigned n){editor.captureFrame(f,n);};
        view.smokeInput=[&](unsigned frame){if(componentSmoke) editor.componentSmokeInput(frame); else editor.smokeInput(frame);};
        view.smokeAfterFrame=[&](unsigned frame){if(componentSmoke) editor.componentSmokeAfterFrame(frame); else editor.smokeAfterFrame(frame);};
        return graphics::run(std::move(view),std::move(options));
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
