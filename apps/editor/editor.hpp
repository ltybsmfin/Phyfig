#pragma once
#include "workspace.hpp"
#include "imgui.h"
#include "background_job.hpp"
#include <map>
#include <set>

namespace graphics { struct Options; }

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
    void pollJobs();
    void setupExport(const Json& snapshot);
    void exportFrame(unsigned frame,double fps);
    void drawExport();
    // Host hook: collect the actual exported text and the largest text pixel
    // size so the host can rasterize the needed glyphs for the export process.
    void configureExportFonts(graphics::Options& options);
private:
    // One recorded canvas camera per scope. Export "current" replays it instead
    // of borrowing whatever the GUI happens to show now.
    struct CanvasView {
        bool ready=false;
        std::string scope;
        float width=0,height=0;                 // logical ImGui canvas pixels
        float physicalWidth=0,physicalHeight=0; // framebuffer pixels
        float zoom=0,referenceZoom=0;
        float framebufferScaleX=1,framebufferScaleY=1;
        ImVec2 origin{},pan{};                  // canvas-local, +y up
        bool grid=false,trails=false;
    };
    struct TextSample { std::string object; std::string text; };
    CanvasView recordedView(const std::string& scope) const;
    CanvasView fitView(const std::string& scope,float width,float height);
    void invalidateRecordedViews(const std::string& scope=std::string());
    void publishFrameView();
    void publishPresentedView();
    Json frameViewJson(const CanvasView& view,const char* source) const;
    void refreshTextWarnings();
    void collectTextSamples(const Document& doc,bool componentScope,std::vector<TextSample>& out) const;
    float exportPixelScale(float targetWidth,float targetHeight) const;
    void beginColorEdit();
    void commitColorEdit();
    void cancelColorEdit(const std::string& reason=std::string());
    void beginStepEdit();
    void commitStepEdit();
    void cancelStepEdit(const std::string& reason=std::string());
    BackgroundJob formulaJob_,exportJob_;
    Json formulaRequest_;
    std::string formulaSource_,formulaScope_,formulaObject_;
    uint64_t formulaRevision_=0,workspaceGeneration_=0,componentGeneration_=0,formulaGeneration_=0;
    char latexInput_[4097]="\\vec{v}=\\frac{\\Delta \\vec{r}}{\\Delta t}";
    char pythonPath_[2048]="python";
    char exportPath_[2048]="exports/animation.mp4",ffmpegPath_[2048]="ffmpeg";
    int exportWidth_=1280,exportHeight_=720,exportFps_=24,exportFormat_=0;
    bool exportDialogOpen_=false,exportOverwrite_=false;
    // 0 = current composition (default), 1 = fit the requested viewport.
    int exportViewMode_=0;
    bool exportMatchCanvas_=true;
    ImVec2 lastCanvasStart_{};
    Json exportSnapshot_;
    Json jobRequest(const Json&,const std::string&);
    void formulaPanel(const Json& object);
    void jobsPanel();
    void exportDialog();
    void syncFormulaResources();
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
    // Text size and formula scale are authored in screen pixels, so they only
    // match the world-space figures at the zoom they were designed for. This
    // baseline keeps the drawing self-similar: zooming scales everything,
    // it never changes the relative layout.
    float referenceZoom_=0;
    ImVec2 origin_{}, pan_{}, dragStart_{}, dragWorld_{}, dragPreview_{};
    uint64_t dragRevision_=0;
    bool grid_=false, trails_=true, dragging_=false, dirty_=false;
    bool closeDialog_=false, quit_=false;
    bool smokeCloseCancelled_=false;
    ImVec2 closeCancelCenter_{};
    bool showRequested_=false, followMcp_=true, historyOpen_=false;
    uint64_t drawnRevision_=0, historyAfter_=0;
    double drawnTime_=0;
    Json viewState_={{"visible",false},{"minimized",false},{"presentedFrames",0},{"presentedRevision",nullptr},{"presentedTime",nullptr},
        {"canvasReady",false},{"viewportSource",nullptr},{"canvasWidth",nullptr},{"canvasHeight",nullptr},{"zoom",nullptr},
        {"referenceZoom",nullptr},{"origin",nullptr},{"pan",nullptr},{"scope",nullptr},{"presented",nullptr},
        {"view",{{"source","live"},{"canvasReady",false}}},{"textWarnings",Json::array()},{"textWarningCount",0}};
    Json historySelection_;
    unsigned nextId_=1;
    ImVec2 circleButton_{};
    ImVec2 historySliderStart_{},historySliderEnd_{};
    std::string smokeCreated_;
    using Renderer=std::function<void(ImDrawList*,const Json&,State,ImU32)>;
    std::map<std::string,Renderer> renderers_;
    ImVec2 screen(double x,double y) const;
    ImVec2 world(ImVec2 p) const;
    float canvasScale() const;
    void resetPixelReference(float zoom);
    float strokeWidth(const Json& properties) const;
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
    std::map<std::string,CanvasView> recordedViews_;
    CanvasView lastFrameView_;
    bool lastFrameViewValid_=false;
    Json textWarnings_=Json::array();
    std::string textWarningKey_;
    bool textWarningValid_=false;
    bool colorEditing_=false,stepEditing_=false;
    std::string colorEditTarget_,stepEditTarget_;
    uint64_t colorEditRevision_=0,stepEditRevision_=0;
    float colorPreview_[4]={1,1,1,1};
    int stepPreview_=0;
    // U2/U3 real-widget smoke input. Rectangles are captured from the live
    // ColorEdit4/picker widgets while drawing; no absolute screen coordinate is
    // ever guessed. Armed only by the --smoke-test hook.
    bool smokeUiArmed_=false,smokeColorReady_=false,smokePickerReady_=false;
    ImVec2 smokeColorCenter_{},smokePickerMin_{},smokePickerSize_{};
    Json smokeColorBaseline_;
    uint64_t smokeColorRevision_=0,smokeColorHistory_=0;
    Json smokeViewBaseline_;
};
}
