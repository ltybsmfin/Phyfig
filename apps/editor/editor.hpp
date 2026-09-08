#pragma once
#include "workspace.hpp"
#include "imgui.h"
#include <map>
#include <set>

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
    void componentSmokeInput(unsigned frame);
    void componentSmokeAfterFrame(unsigned frame);
    void animationSmokeInput(unsigned frame);
    void animationSmokeAfterFrame(unsigned frame);
    void captureFrame(unsigned frame,unsigned count);
    Document& document() { return workspace_.document(); }
    void startJournal(const std::string& directory) { workspace_.startJournal(directory); }
    bool requestClose();
    bool shouldClose() const { return quit_; }
private:
    Workspace workspace_;
    std::string scope_="scene", drawnScope_="scene";
    Document& active() { return workspace_.document(scope_); }
    void switchScope(const std::string& scope);
    void componentPanel();
    void fitCanvas(ImVec2 start,ImVec2 size,const Json& objects);
    std::pair<ImVec2,ImVec2> bounds(const Json& object,State state);
    bool fitRequested_=false;
    ImVec2 lastCanvasSize_{};
    std::set<std::string> grouped_;
    char componentId_[128]="new_component", metadata_[32768]{};
    uint64_t metadataRevision_=~uint64_t(0);
    char scenePath_[2048]="scenes/my-scene.json", componentPath_[2048]="assets/components/my-component.json";
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
    ImVec2 historySliderStart_{},historySliderEnd_{};
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
    void animationPanel();
    void applyLayout();
    void storeLayout();
    void projectDialog();
    bool animationWorkspace_=false,styleReady_=false;
    float sidebarWidth_=320,bottomHeight_=260;
    char projectPath_[2048]="projects/untitled.physproj";
    std::string channel_="x",animationObject_;
    char animationJson_[65537]{};
    uint64_t animationRevision_=~uint64_t(0);
    int selectedKey_=-1;
    bool keyDragging_=false;
    bool keyMoved_=false;
    double keyOldTime_=0,keyTime_=0,keyValue_=0;
    uint64_t keyRevision_=0;
    ImVec2 curveStart_{},curveEnd_{};
    double curveMin_=-1,curveMax_=1;
    bool projectDialogOpen_=false;
    bool timeSeeking_=false;
};
}
