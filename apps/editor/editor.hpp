#pragma once
#include "document.hpp"
#include "imgui.h"
#include <map>

namespace physics_editor {
class Editor {
public:
    Editor();
    void draw(float dt);
    Json dispatch(const Json& request,const std::string& source="human");
    bool takeShowRequest() { const bool requested=showRequested_; showRequested_=false; return requested; }
    void windowState(bool visible,bool minimized,unsigned frames);
    void presented();
    void smokeScenario(int scenario);
    void smokeInput(unsigned frame);
    void smokeAfterFrame(unsigned frame);
    void smokeCheck();
    void captureFrame(unsigned frame,unsigned count);
    Document& document() { return document_; }
    bool requestClose();
    bool shouldClose() const { return quit_; }
private:
    Document document_;
    std::string selected_="ball", status_="就绪 · 场景单位 m / s";
    char path_[2048]="scenes/my-scene.json";
    char jsonEdit_[16384]{};
    std::string editedId_;
    uint64_t editedRevision_=~uint64_t(0);
    float zoom_=35;
    ImVec2 origin_{}, pan_{}, dragStart_{}, dragWorld_{}, dragPreview_{};
    uint64_t dragRevision_=0;
    bool grid_=false, trails_=true, dragging_=false, dirty_=false;
    bool closeDialog_=false, quit_=false;
    bool smokeCloseCancelled_=false;
    ImVec2 closeCancelCenter_{};
    bool showRequested_=false, followMcp_=true, historyOpen_=false;
    uint64_t drawnRevision_=0, historyAfter_=0;
    double drawnTime_=0;
    Json viewState_={{"visible",false},{"minimized",false},{"presentedFrames",0},{"presentedRevision",nullptr},{"presentedTime",nullptr}};
    Json historySelection_;
    unsigned nextId_=1;
    ImVec2 circleButton_{};
    std::string smokeCreated_;
    using Renderer=std::function<void(ImDrawList*,const Json&,State,ImU32)>;
    std::map<std::string,Renderer> renderers_;
    ImVec2 screen(double x,double y) const;
    ImVec2 world(ImVec2 p) const;
    void arrow(ImDrawList*,ImVec2,ImVec2,ImU32,float);
    void palette();
    void inspector();
    void canvas(ImVec2 size);
    void execute(Json commands);
    void attempt(const Json& request);
    void initializeRenderers();
    void historyPanel();
};
}
