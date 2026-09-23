#pragma once
#include "workspace.hpp"
#include "activity_log.hpp"
#include "imgui.h"
#include "background_job.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace graphics { struct Options; }

namespace physics_editor {
// 按像素宽度把 UTF-8 文本省略到最后一个完整码点（保留开头——方法名/路径开头最有
// 信息量），追加 ASCII 省略号。右侧日志栏只有 240~340 px，Selectable 又不会自动
// 换行：不省略就会被栏右边缘直接裁掉。纯展示辅助，单个码点从不在中间截断。
// 先取出码点边界再做宽度二分：日志列表每帧对几百条记录调用，线性试测量会变成
// 每帧上万次 CalcTextSize。
inline std::string elideUtf8ToWidth(const std::string& text,float maxWidth) {
    if(maxWidth<=8.f || ImGui::CalcTextSize(text.c_str()).x<=maxWidth) return text;
    const char* tail="...";
    const float budget=maxWidth-ImGui::CalcTextSize(tail).x;
    std::vector<size_t> bounds;
    bounds.reserve(text.size()+1);
    for(size_t i=0;i<text.size();) {
        const unsigned char lead=static_cast<unsigned char>(text[i]);
        const size_t length=lead<0x80?1:((lead&0xE0)==0xC0?2:((lead&0xF0)==0xE0?3:((lead&0xF8)==0xF0?4:1)));
        i+=length>text.size()-i?text.size()-i:length;
        bounds.push_back(i);
    }
    size_t low=0,high=bounds.size();  // low 之前一定放得下，high 起一定放不下
    while(low<high) {
        const size_t mid=(low+high)/2;
        if(ImGui::CalcTextSize(text.substr(0,bounds[mid]).c_str()).x<=budget) low=mid+1;
        else high=mid;
    }
    return text.substr(0,low?bounds[low-1]:0)+tail;
}
// 后台任务进度面板应该画在哪里：日志栏内（activity）/ 兜底固定浮层（overlay）/ 不画
// （none）。draw() 与 activityPanel() 共用这一个判断，冒烟测试也直接断言它：日志栏
// 收起或窄到只剩按钮条时，进度必须有真正可见的落点，不能像以前那样留在布局流末尾
// 被状态行与播放控件挤出屏幕（那样连「取消任务」都点不到）。
inline const char* jobPanelSurface(bool running,bool activityShown,bool activityBar) {
    if(!running) return "none";
    return (!activityShown||activityBar)?"overlay":"activity";
}
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
    // ------------------------------------------------------------------
    // 操作日志（右侧额外侧栏）。它与底部「编辑历史」共用文档但语义不同：
    // 撤销历史 = 可回退的文档状态；操作日志 = 本会话实际发生过什么。
    //
    // 接线约定：界面与 MCP 都经 dispatch() 自动入日志，不需要各自埋点。
    // 异步后台任务（公式编译 / 视频导出）的终态由 pollJobs() 调用
    // recordActivityJob 追加一条**独立**记录（startedId==0）：
    //   * 发起请求已经由 dispatch() 记录过，这里绝不重复记录请求；
    //   * 终态记录保留请求的 source（human/mcp）与 scope（scene/component），
    //     并把 job id 与原始 worker 状态放进 detail；
    //   * 每个任务运行只追加一次（按 BackgroundJob::state["id"] 去重）。
    // startedId != 0 的回填路径仍然保留，供「必须改写同一行」的调用方使用。
    // ------------------------------------------------------------------
    uint64_t recordActivity(const std::string& method,const std::string& scope,const std::string& source,
                            bool ok,const std::string& summary=std::string(),
                            const std::string& error=std::string(),const Json& detail=Json::object());
    // 异步后台任务终态；job worker 只需在 pollJobs() 时调用这一条。
    struct JobOutcome {
        std::string kind;           // formula | export
        std::string scope;          // scene | component
        std::string source;         // human | mcp
        std::string status;         // completed | failed | cancelled
        std::string error;
        std::string summary;
        std::string jobId;          // BackgroundJob state["id"]，仅用于详情
        uint64_t startedId=0;       // recordActivity 返回的 id（0 = 追加一条新记录）
    };
    uint64_t recordActivityJob(const JobOutcome& outcome);
    bool activityLogOpen() const { return activityOpen_; }
    void setActivityLogOpen(bool open) { activityOpen_=open; }private:
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
    // 导出任务的来源与范围（human/mcp + scene/component）：终态记录必须保留它们，
    // 与 formula 任务的 formulaSource_/formulaScope_ 同义。
    std::string exportSource_="human",exportScope_="scene";
    // 异步任务终态上报去重：值是该次运行的 BackgroundJob::state["id"]（每次 start()
    // 都会换新 id），因此 pollJobs() 每帧调用也只会追加一条终态记录。
    std::string formulaJobReported_,exportJobReported_;
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
    void writePanel(const Json& object);
    // 后台任务进度：日志栏内与兜底浮层共用同一份正文绘制（jobsPanel），两处永远
    // 显示同一份 progress，不会各写一套。anyJobRunning() 决定是否需要进度界面。
    void jobsPanel();
    void jobsFallbackOverlay(ImVec2 anchor,float width);
    bool anyJobRunning() const;
    // 进度面板本帧实际画在哪里：日志栏内（activity）/ 独立浮层（overlay）/ 没画
    // （none）。只读诊断，get_activity_log 会带出去，便于验证「日志栏收起时进度与
    // 取消仍然可见」而不是只靠肉眼判断。
    Json jobSnapshot() const;
    // 异步后台任务的终态记录（完成/失败/取消）；每个任务运行只追加一次。
    void reportJobOutcome(const char* kind,BackgroundJob& job,const std::string& source,
                          const std::string& scope,std::string& reported);
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
    // --- 操作日志侧栏（记录） ---
    ActivityLog activityLog_;
    bool activityOpen_=true,activityFollow_=true,activityShowUi_=true,activityShowMcp_=true,activityShowFailed_=true;
    bool activityShowQuiet_=false,activityDetailOpen_=false;
    uint64_t activityAfter_=0,activityLastId_=0,activityNewestSeen_=0;
    float activityWidth_=340.f;
    EditorColumns activityColumns_{};
    // 「编辑画布」宽度（不含只读日志栏）。文字/公式的标称像素基准只在
    // referenceZoom_<=0（刚替换/载入文档）时由它拟合，因此日志栏的展开/收起/
    // 拖动不会改变导出像素。见 computeEditorColumns().editingDrawing。
    float editingCanvasWidth_=0.f;
    // --- 操作日志侧栏（显示状态，供 get_activity_log 只读查询） ---
    bool activityVisible_=false;      // 本帧是否真的画出来了（独立于开关）
    bool activityCollapsedBar_=false; // 太窄：只显示按钮条
    bool activityFollowing_=false;    // 本帧是否正在自动跟随末尾
    // 后台任务进度面板本帧的呈现位置（见 jobSnapshot）：activity / overlay / none。
    // 浮层是「日志栏收起/只剩按钮条」时的兜底：它必须画在真正可见的位置，不能像
    // 以前那样留在正常布局流末尾被状态行挤出屏幕。
    std::string jobsSurface_="none";
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
    void activityPanel(EditorColumns columns);
    void recordActivityLine(const Json& request,const std::string& source,bool ok,
                            const std::string& error,const std::string& scope);
    void storeActivityState();
    void applyActivityState(const Json& layout);
    // 冒烟测试用的即时快照：侧栏本帧的可见状态 + 有界记录概要。
    Json activitySnapshot() const;
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
