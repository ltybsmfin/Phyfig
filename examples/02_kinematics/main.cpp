#include "lesson.hpp"
#include "vulkan_host.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    graphics::Options options; options.title=L"运动学实验室 · 斜抛运动 / Vulkan + C++";
    options.escapeCloses=true;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--smoke-test") { options.smoke=true; options.validation=true; }
        else if(arg=="--validation") options.validation=true;
        else if(arg=="--capture-animation") options.capture=true;
        else if(arg=="--help") { std::cout<<"Kinematics [--validation] [--smoke-test | --capture-animation]\n"; return 0; }
        else { std::cerr<<"Unknown argument: "<<arg; return 1; }
    }
    KinematicsLesson lesson;
    graphics::View view;
    view.draw=[&](float dt){lesson.draw(dt);};
    view.captureFrame=[&](unsigned f,unsigned n){lesson.captureFrame(f,n);};
    view.smokeInput=[&](unsigned frame) {
        if(frame==3 || frame==8 || frame==13 || frame==4 || frame==9 || frame==14) {
            const int step=frame<8?1:(frame<13?2:3);
            auto& io=ImGui::GetIO(); const auto p=lesson.stepButtonCenter(step);
            io.AddMousePosEvent(p.x,p.y); io.AddMouseButtonEvent(0,frame==3 || frame==8 || frame==13);
        }
    };
    view.smokeAfterFrame=[&](unsigned frame) {
        if(frame==18 && lesson.step()!=3) throw std::runtime_error("Lesson button input test failed");
        if(frame>=72 && frame<=104 && (frame-72)%8==0) lesson.smokeScenario((frame-72)/8);
        if(frame==110) lesson.smokeCheck();
    };
    return graphics::run(std::move(view),std::move(options));
}
