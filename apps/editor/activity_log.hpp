#pragma once
// 编辑器右侧「操作日志」侧栏的数据核心（独立于文档撤销历史与磁盘 operation
// journal）。它记录来自人类界面和 MCP 的**实际请求**：谁发起、作用在哪个
// 范围（主场景/组件）、什么时候、做了什么、成功还是失败。
//
// 设计边界（与撤销历史语义不同，不要合并）：
//   * 撤销历史属于 Document：它保留可恢复的文档状态、可回退、可预览。
//   * 操作日志属于会话：它只描述「发生过什么」，不提供回退，也不显示状态。
//   * 轮询类只读请求（get_scene/get_job/get_view_state …）默认不入日志，
//     否则一次 MCP 轮询就能把 500 条容量刷满；但它们一旦失败必须可见。
//
// 本文件是 header-only、无 ImGui 依赖，便于独立编译测试。
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

namespace physics_editor {
using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// 一条操作日志。所有自由文本都已按字节有界，界面不会因为一条巨大 JSON 卡住。
// ---------------------------------------------------------------------------
struct ActivityItem {
    uint64_t id = 0;                 // 单调递增，即可见顺序也是 after 游标
    std::string utc;                 // 2026-01-02T03:04:05.678Z
    uint64_t millis = 0;             // 来自纪元，仅用于展示间隔
    std::string source;              // human | mcp | system | job
    std::string scope;               // scene | component | "-"
    std::string method;              // apply / save_project / export_animation …
    std::string status;              // ok | error | warning
    bool query = false;              // 默认静默的只读查询
    std::string summary;             // 人类可读的一行摘要
    std::string error;               // status != ok 时的原因
    Json detail = Json::object();    // 有界详情（args / ops / job id …）
    bool detailTruncated = false;    // detail 因体积被替换为说明对象
    unsigned repeats = 1;            // 连击合并次数（拖动滑杆等）

    ActivityItem() = default;
    ActivityItem(const ActivityItem&) = default;
    ActivityItem(ActivityItem&&) = default;
    ActivityItem& operator=(const ActivityItem&) = default;
    ActivityItem& operator=(ActivityItem&&) = default;

    bool failed() const { return status == "error"; }
    bool warning() const { return status == "warning"; }
    bool succeeded() const { return status == "ok"; }
};

class ActivityLog {
public:
    // 500 条足以覆盖一次会话的可见操作；超过后丢弃最旧记录。
    static constexpr size_t kCapacity = 500;
    static constexpr size_t kSummaryBytes = 240;
    static constexpr size_t kErrorBytes = 320;
    static constexpr size_t kDetailBytes = 4096;
    // 同一方法在同一毫秒窗口内连续成功会合并计数，避免拖动滑杆刷屏。
    static constexpr uint64_t kCoalesceMillis = 2000;
    // 一次 query 最多列出多少个被改写的 id（updatedCount 仍是完整数量）。
    static constexpr size_t kMaxUpdatedIds = 32;

    void clear() {
        items_.clear();
        touched_.clear();
        nextId_ = 1;
        dropped_ = 0;
        ++epoch_;
    }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    uint64_t lastId() const { return nextId_ - 1; }
    // 会话代次：clear() 会让 id 从头开始，游标必须连代次一起看。只增不减。
    uint64_t epoch() const { return epoch_; }
    // 最旧的仍在内存中的记录 id（被容量上限淘汰后前进）。仅用于界面提示。
    uint64_t firstAvailableId() const { return items_.empty() ? nextId_ : items_.front().id; }
    // 因容量上限被淘汰的记录数（不是历史游标，仅供界面提示）。
    uint64_t dropped() const { return dropped_; }
    const std::deque<ActivityItem>& items() const { return items_; }

    // ---------------------------------------------------------------------
    // 写入。返回这条记录最终的 id（合并连击时是既有记录的 id）。
    // 三个入口刻意区分名字，避免 bool/status 之间发生隐式转换：
    //   record(...)        常规成功/失败
    //   recordDetailed(...)需要显式状态（warning）或显式时间戳（测试）时
    //   recordRequest(...) 直接记录一条 dispatch 请求
    // ---------------------------------------------------------------------
    uint64_t record(const std::string& method, const std::string& scope, const std::string& source,
                    bool ok, const std::string& summary, const std::string& error = std::string(),
                    const Json& detail = Json::object()) {
        return recordDetailed(method, scope, source, ok ? "ok" : "error", summary, error, detail,
                              utcNow(), epochMillis());
    }
    uint64_t recordDetailed(const std::string& method, const std::string& scope, const std::string& source,
                            const std::string& state, const std::string& summary, const std::string& error,
                            const Json& detail = Json::object(),
                            const std::string& utc = utcNow(), uint64_t millis = epochMillis()) {
        const std::string normalized = normalizeStatus(state);
        const std::string text = truncate(summary, kSummaryBytes);
        const std::string reason = truncate(error, kErrorBytes);
        // 同一操作的连击（拖动滑杆、反复点 +1）合并成一条并计数，而不是刷满日志。
        if (normalized == "ok" && !items_.empty()) {
            ActivityItem& last = items_.back();
            if (last.status == "ok" && last.source == source && last.scope == scope &&
                last.method == method && last.summary == text && sameDetail(last, detail) &&
                millis >= last.millis && millis - last.millis <= kCoalesceMillis) {
                last.millis = millis;
                last.utc = truncate(utc, 64);
                ++last.repeats;
                // 合并会改动一条已经被发出去的记录：登记它，query() 才能告诉
                // 「after 已经越过这条记录」的客户端它变了。
                touch(last.id);
                return last.id;
            }
        }
        ActivityItem item;
        item.id = nextId_++;
        item.utc = truncate(utc, 64);
        item.millis = millis;
        item.source = truncate(source, 32);
        item.scope = truncate(scope, 32);
        item.method = truncate(method, 64);
        item.status = normalized;
        item.query = isQueryMethod(method);
        item.summary = text;
        item.error = reason;
        item.detail = boundedDetail(detail);
        item.detailTruncated = item.detail.is_object() && item.detail.contains("omitted");
        items_.push_back(std::move(item));
        while (items_.size() > kCapacity) {
            const uint64_t evicted = items_.front().id;
            items_.pop_front();
            forgetTouch(evicted);
            ++dropped_;
        }
        return items_.back().id;
    }

    // 撤销/重做这类「同一事务的从属记录」用 request.detail 区分，不与主动作合并。
    // scopeOverride 用于记录「实际生效范围」：load/save_project 的请求本身可能不带 scope。
    uint64_t recordRequest(const Json& request, const std::string& source, bool ok,
                           const std::string& error = std::string(), const std::string& summary = std::string(),
                           const std::string& scopeOverride = std::string()) {
        return recordRequestDetailed(request, source, ok ? "ok" : "error", error, summary, scopeOverride);
    }
    // 需要「成功但有附加警告」（例如会话日志写盘失败）时用这个入口。
    uint64_t recordRequestDetailed(const Json& request, const std::string& source, const std::string& state,
                                   const std::string& error = std::string(),
                                   const std::string& summary = std::string(),
                                   const std::string& scopeOverride = std::string()) {
        const std::string method = request.value("method", std::string("invalid"));
        const std::string scope = scopeOverride.empty()
            ? request.value("scope", std::string("-")) : scopeOverride;
        Json detail = {{"method", method}, {"scope", scope}};
        if (request.contains("args")) detail["args"] = request["args"];
        return recordDetailed(method, scope, source, state,
                              summary.empty() ? describeSummary(method, request) : summary, error, detail);
    }
    // 后台任务终态（异步 job 完成/失败）。startedId 为发起记录 id，0 表示不合并。
    uint64_t recordJob(const std::string& kind, const std::string& jobId, const std::string& scope,
                       const std::string& source, const std::string& status, const std::string& error,
                       const std::string& summary, uint64_t startedId = 0) {
        const std::string state = status == "completed" ? "ok" : (status == "cancelled" ? "warning" : "error");
        const std::string text = summary.empty()
            ? (kind + " 任务 " + (status == "completed" ? "完成" : status == "cancelled" ? "已取消" : "失败"))
            : summary;
        if (startedId != 0) {
            const auto it = std::find_if(items_.begin(), items_.end(),
                [&](const ActivityItem& item) { return item.id == startedId; });
            if (it != items_.end()) {
                it->status = state;
                it->error = truncate(error, kErrorBytes);
                it->summary = truncate(text, kSummaryBytes);
                if (it->detail.is_object()) {
                    it->detail["status"] = status;
                    if (!error.empty()) it->detail["error"] = truncate(error, kErrorBytes);
                }
                it->millis = epochMillis();
                it->utc = utcNow();
                // 终态改写了一条旧记录：登记的游标语义与连击合并完全相同。
                touch(it->id);
                return it->id;
            }
        }
        const Json detail = {{"kind", kind}, {"job", jobId}, {"status", status}, {"startedId", startedId}};
        return recordDetailed(kind + ".job", scope.empty() ? std::string("-") : scope,
                              source.empty() ? "job" : source, state, text, error, detail);
    }

    // ---------------------------------------------------------------------
    // 读取：after 之后的记录，最多 limit 条，升序。全部字段有界。
    //
    // 游标语义（客户端必须按这三条读，否则会静默丢事件）：
    //   * entries/nextAfter/hasMore 只覆盖「新记录」：after 是排他的。
    //   * clear() 会让 id 重新从 1 开始，因此 after > lastId 时 `reset` 为真，
    //     此时本页从 0 重新开始返回，客户端必须丢弃旧游标（`epoch` 是同一事实
    //     的单调版本号，可以只比较代次）。
    //   * 被容量上限淘汰的记录会推高 firstAvailableId，`missed` 报告游标之后
    //     已经不在内存里的记录数：那不是「没有新事件」，而是「丢了一段」。
    //   * 连击合并（以及 recordJob 的 startedId 终态回填）会改写一条**已经发
    //     出去**的记录，它的 id 不会前进。`updatedIds` 列出这些 id，客户端若
    //     缓存过它们就必须重读；`updated` 是同一事实的布尔量。
    // ---------------------------------------------------------------------
    Json query(uint64_t after = 0, size_t limit = 100) const {
        if (limit < 1) limit = 1;
        if (limit > kCapacity) limit = kCapacity;
        Json entries = Json::array();
        const bool reset = after > lastId();
        const uint64_t from = reset ? 0 : after;
        uint64_t cursor = from;
        bool more = false;
        for (const auto& item : items_) {
            if (item.id <= from) continue;
            if (entries.size() >= limit) { more = true; break; }
            entries.push_back(itemJson(item));
            cursor = item.id;
        }
        const uint64_t firstAvailable = items_.empty() ? nextId_ : items_.front().id;
        // from+1 .. firstAvailable-1 已经没有记录了：明确报告丢了多少，而不是让
        // 客户端把「没有 entries」理解成「没有新事件」。
        const uint64_t missed = firstAvailable > from + 1 ? firstAvailable - from - 1 : 0;
        Json updatedIds = Json::array();
        size_t updatedCount = 0;
        for (const uint64_t id : touched_) {
            if (id > after) continue;   // 新记录会由 entries 给出，不算「改写」
            ++updatedCount;
            if (updatedIds.size() < kMaxUpdatedIds) updatedIds.push_back(id);
        }
        return {{"entries", entries},
                {"count", entries.size()},
                {"lastId", lastId()},
                {"firstAvailableId", firstAvailable},
                {"after", after},
                {"nextAfter", reset ? (entries.empty() ? 0 : cursor) : cursor},
                {"hasMore", more},
                {"reset", reset},
                {"epoch", epoch_},
                {"missed", missed},
                {"updated", updatedCount > 0},
                {"updatedIds", updatedIds},
                {"updatedCount", updatedCount},
                {"capacity", kCapacity},
                {"dropped", dropped_}};
    }
    Json itemJson(const ActivityItem& item) const {
        Json value = {{"id", item.id}, {"utc", item.utc}, {"millis", item.millis},
                      {"source", item.source}, {"scope", item.scope}, {"method", item.method},
                      {"status", item.status}, {"query", item.query}, {"summary", item.summary},
                      {"error", item.error}, {"repeats", item.repeats},
                      {"detail", item.detail}, {"detailTruncated", item.detailTruncated}};
        return value;
    }

    // ---------------------------------------------------------------------
    // 工具（静态纯函数，便于单独测试）
    // ---------------------------------------------------------------------
    // 查询类方法默认静默：它们通常在轮询，成功时没有信息量。
    static bool isQueryMethod(const std::string& method) {
        static const char* kQueries[] = {"get_scene", "get_catalog", "get_history", "get_timeline",
            "get_project", "get_view_state", "get_job", "get_formulas", "get_activity_log",
            "sample_animation"};
        for (const char* name : kQueries) if (method == name) return true;
        return false;
    }
    static std::string normalizeStatus(const std::string& status) {
        if (status == "ok" || status == "success") return "ok";
        if (status == "warning" || status == "warn") return "warning";
        return "error";
    }
    // 后台任务的终态判定：idle/running 不是终态；取消也是终态（而且 cancel() 之后
    // BackgroundJob::running() 立刻为 false，poll() 不会再返回 true，因此调用方必须
    // 无条件检查状态而不是只看 poll() 的返回值）。
    static bool isTerminalJobStatus(const std::string& status) {
        return status == "completed" || status == "failed" || status == "cancelled";
    }
    // 终态上报的去重键：同一次运行只上报一次。BackgroundJob::start() 每次都会重置
    // state 并换一个新的 "id"，所以 id 就是运行的代次；极端情况下没有 id 时退回状态名。
    static std::string jobReportKey(const std::string& id, const std::string& status) {
        return id.empty() ? status : id;
    }
    static std::string utcNow() {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
        std::tm utc{};
#if defined(_WIN32)
        gmtime_s(&utc, &seconds);
#else
        gmtime_r(&seconds, &utc);
#endif
        char text[48];
        std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                      static_cast<int>(ms.count()));
        return text;
    }
    static uint64_t epochMillis() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    // 按字节截断，并且不会把一个 UTF-8 码点劈成半个。
    static std::string truncate(const std::string& text, size_t maxBytes) {
        if (text.size() <= maxBytes) return text;
        size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        std::string result = text.substr(0, cut);
        result += "…";
        return result;
    }
    // 只读请求的摘要：把参数压成一行，避免把整份文档贴进日志。
    static std::string describeRequest(const Json& request) {
        const std::string method = request.value("method", std::string("invalid"));
        const Json args = request.value("args", Json::object());
        if (method == "apply") {
            std::string text;
            const Json commands = args.value("commands", Json::array());
            for (const auto& command : commands) {
                if (!text.empty()) text += " · ";
                text += command.value("op", std::string("?"));
                if (command.contains("id")) text += " " + command["id"].dump();
                if (text.size() > kSummaryBytes) return truncate(text + " …", kSummaryBytes);
            }
            return truncate(text.empty() ? std::string("apply（空命令）") : text, kSummaryBytes);
        }
        static const char* kPathArgs[] = {"path", "output", "id", "latex", "kind", "view_mode"};
        std::string text = method;
        for (const char* key : kPathArgs) {
            if (!args.contains(key) || !args[key].is_string()) continue;
            const std::string value = args[key].get<std::string>();
            if (key == std::string("latex")) { text += " latex=" + std::to_string(value.size()) + "B"; continue; }
            text += " " + std::string(key) + "=" + value;
            if (text.size() > kSummaryBytes) return truncate(text, kSummaryBytes);
        }
        return truncate(text, kSummaryBytes);
    }
    static std::string describeSummary(const std::string& method, const Json& request) {
        return describeRequest({{"method", method}, {"args", request.value("args", Json::object())}});
    }

private:
    // 连击判定必须比较详情**内容**。只比字节数会把长度相同、内容不同的两个操作
    // 错误合并成一条（例如同一方法反复作用在不同对象上），那等于篡改历史。
    // 被体积上限省略的巨型详情无法比较，一律不合并：宁可多出一行，也不能把两件
    // 不同的事说成一件。合并只在「同方法/同来源/同范围/同摘要/同详情」时发生，
    // 因此这次深度比较平均每帧最多发生一次（窗口内的连续同操作），不是每条记录。
    bool sameDetail(const ActivityItem& last, const Json& detail) const {
        if (last.detailTruncated) return false;
        const Json bounded = boundedDetail(detail);
        if (bounded.is_object() && bounded.contains("omitted")) return false;
        return last.detail == bounded;
    }
    // 改写登记的 id 只保留有界数量，并保持「最近改写在后」的顺序。
    void touch(uint64_t id) {
        const auto found = std::find(touched_.begin(), touched_.end(), id);
        if (found != touched_.end()) touched_.erase(found);
        touched_.push_back(id);
        while (touched_.size() > kCapacity) touched_.pop_front();
    }
    void forgetTouch(uint64_t id) {
        const auto found = std::find(touched_.begin(), touched_.end(), id);
        if (found != touched_.end()) touched_.erase(found);
    }
    // detail 也有体积上限：宁可丢掉不可读的巨型 JSON，也不能让界面每帧序列化它。
    Json boundedDetail(const Json& detail) const {
        const std::string dumped = detail.dump();
        if (dumped.size() <= kDetailBytes) return detail;
        // 巨型 JSON（例如整份文档）宁可省略，也不让界面每帧去序列化它。
        return Json{{"omitted", true}, {"bytes", dumped.size()},
                    {"note", "详情超过 " + std::to_string(kDetailBytes) + " 字节，已省略"}};
    }

    std::deque<ActivityItem> items_;
    // 创建之后又被改写（连击合并、job 终态回填）的记录 id；query() 用它告诉
    // 「游标已经越过这条记录」的客户端必须重读。
    std::deque<uint64_t> touched_;
    uint64_t nextId_ = 1;
    uint64_t dropped_ = 0;
    uint64_t epoch_ = 0;
};

// ---------------------------------------------------------------------------
// 三列布局：左图元/属性栏、中画布、右操作日志栏。
// 纯函数，无 ImGui 依赖。优先级：画布（>= minDrawing）> 左侧编辑栏 > 日志栏。
// 也就是说窄窗口先牺牲只读的日志栏（先退化成按钮条，再整栏让位），最后才动承载
// 实际编辑功能的左栏；任何情况下三列之和不超过窗口，画布不会小于 minDrawing。
// ---------------------------------------------------------------------------
struct EditorColumns {
    float sidebar = 0.f;
    float activity = 0.f;
    float drawing = 0.f;
    // 「编辑画布」宽度：只把画布、左侧编辑栏和它们之间的分隔条/空隙算进去，
    // 不含只读的操作日志栏。文字与公式的标称像素基准（referenceZoom）由这个宽度
    // 拟合得到，所以展开/收起/拖动日志栏都不会改变同一个文档的导出像素。
    // 注意它是**基准几何**而不是布局：窄窗口下可以大于实际画布。
    float editingDrawing = 0.f;
    float sidebarSplitter = 0.f;
    float activitySplitter = 0.f;
    bool activityBar = false;   // 日志栏太窄：只画一行按钮条
    bool activityShown = false; // 日志栏（完整或按钮条）是否占宽度
};
inline EditorColumns computeEditorColumns(float availableWidth, float sidebarRequested, float activityRequested,
                                          bool activityOpen, float minSidebar = 140.f,
                                          float minActivity = 240.f, float minDrawing = 160.f) {
    EditorColumns columns;
    const float width = std::max(availableWidth, 1.f);
    const float splitter = 6.f; // 分隔条要留有可抓取的宽度
    const float gap = 12.f;     // 画布与相邻栏之间的空隙
    const float bar = 24.f;     // 折叠态只留一条展开按钮
    // 画布是主视图：先扣掉 minDrawing，剩下的才可能分给两侧。
    const float room = std::max(0.f, width - minDrawing - gap);
    const float requestedSidebar = std::clamp(sidebarRequested, minSidebar, 700.f);
    const float requestedLog = std::clamp(activityRequested, minActivity, 700.f);
    const float collapsedBar = width >= 480.f ? bar : 0.f;

    // 一列占用的总宽度：分隔条只在有对应列时占用；日志栏完整显示时才带分隔条。
    const auto cost = [&](float sidebar, float log, bool logBar) {
        return sidebar + (sidebar > 0.f ? splitter : 0.f) + log + (log > 0.f && !logBar ? splitter : 0.f);
    };
    struct LogForm { float width; bool bar; };
    // 候选顺序就是降级顺序：请求宽度 → 下限宽度 → 按钮条 → 整栏让位；
    // 折叠态先保按钮条（它是重新展开日志栏的入口）。
    LogForm logForms[4];
    int logFormCount = 0;
    if (activityOpen) {
        logForms[logFormCount++] = {requestedLog, false};
        if (minActivity < requestedLog - 0.01f) logForms[logFormCount++] = {minActivity, false};
        logForms[logFormCount++] = {width >= 480.f ? bar : 0.f, true};
    } else {
        logForms[logFormCount++] = {collapsedBar, true};
    }
    logForms[logFormCount++] = {0.f, false};

    float sidebar = requestedSidebar;
    float log = 0.f;
    bool logBar = false;
    bool placed = false;
    const auto place = [&](float left, const LogForm& form) {
        if (cost(left, form.width, form.bar) > room + 0.01f) return false;
        sidebar = left; log = form.width; logBar = form.width > 0.f && form.bar; return true;
    };
    // 1) 左栏（真正的编辑功能）按请求宽度，日志栏从完整降到按钮条再到让位。
    for (int i = 0; i < logFormCount && !placed; ++i) placed = place(requestedSidebar, logForms[i]);
    // 2) 仍然放不下：收缩左栏（不低于下限的 3/4），日志栏只保留能放下的形态。
    for (int i = 0; i < logFormCount && !placed; ++i) {
        const LogForm& form = logForms[i];
        const float logCost = form.width > 0.f ? (form.bar ? form.width : form.width + splitter) : 0.f;
        const float left = std::max(0.f, std::min(requestedSidebar, room - logCost - splitter));
        if (left < minSidebar * 0.75f) continue;
        placed = place(left, form);
    }
    // 3) 极窄窗口：左栏也只能整栏让位，把宽度留给日志栏与画布。
    for (int i = 0; i < logFormCount && !placed; ++i) placed = place(0.f, logForms[i]);
    if (!placed) { sidebar = 0.f; log = 0.f; logBar = false; }
    if (sidebar > 0.f && sidebar < minSidebar * 0.75f) sidebar = 0.f; // 不放无意义的窄缝

    columns.sidebar = sidebar;
    columns.sidebarSplitter = sidebar > 0.f ? splitter : 0.f;
    columns.activity = log;
    columns.activityShown = log > 0.f;
    columns.activityBar = logBar;
    columns.activitySplitter = log > 0.f && !logBar ? splitter : 0.f;
    columns.drawing = std::max(1.f, width - columns.sidebar - columns.sidebarSplitter -
        columns.activity - columns.activitySplitter - gap);
    // 编辑画布基准几何：左栏按请求宽度，只扣一个分隔条与画布空隙，与日志栏无关。
    columns.editingDrawing = std::max(1.f, width - requestedSidebar - splitter - gap);
    return columns;
}
}
