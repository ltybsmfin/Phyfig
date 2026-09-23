// 操作日志核心与三列布局的独立单元测试。只用 activity_log.hpp（header-only，
// 不含 ImGui / Vulkan / 窗口），因此不需要 GPU、不需要启动编辑器：
//
//   cl /std:c++17 /EHsc /W4 /utf-8 /I third_party\json /I apps\editor
//      apps\editor\activity_log_test.cpp /Fe:build\activity_log_test.exe
//
// 覆盖：有界容量、after/limit 游标（连击改写、容量淘汰、clear 重置）、真实操作/
// 查询/失败的语义区分、连击合并（同长不同详情绝不合并）、巨型详情与超长文本的按字节
// （且不劈开 UTF-8 码点）截断、后台任务终态记录，以及三列布局在窄窗口下优先保主编辑
// 功能、以及「编辑画布」基准宽度与只读日志栏无关。
#include "activity_log.hpp"
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace physics_editor;

namespace {
int failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::cout << "FAIL: " << what << '\n';
}

void checkEqual(const std::string& got, const std::string& expected, const std::string& what) {
    check(got == expected, what + " (got '" + got + "', expected '" + expected + "')");
}

void checkEqual(uint64_t got, uint64_t expected, const std::string& what) {
    check(got == expected, what + " (got " + std::to_string(got) + ", expected " + std::to_string(expected) + ")");
}

void checkNear(float got, float expected, const std::string& what) {
    check(std::abs(got - expected) < 0.01f,
          what + " (got " + std::to_string(got) + ", expected " + std::to_string(expected) + ")");
}

// 简单 UTF-8 校验：不会出现半个码点（续字节前必须有合法的引导字节）。
bool validUtf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = 0;
        if (lead < 0x80) length = 1;
        else if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        else return false;
        if (i + length > text.size()) return false;
        for (size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        i += length;
    }
    return true;
}

void testRecordAndQuery() {
    ActivityLog log;
    check(log.empty() && log.lastId() == 0, "a new log starts empty");
    const uint64_t first = log.record("apply", "scene", "human", true, "create ball");
    const uint64_t second = log.recordRequest(
        {{"method", "save"}, {"scope", "scene"}, {"args", {{"path", "scenes/my-scene.json"}}}}, "human", true);
    checkEqual(first, 1, "ids start at 1");
    checkEqual(second, 2, "ids increase");
    checkEqual(log.size(), 2, "two records are stored");

    const Json all = log.query(0, 100);
    checkEqual(all["count"].get<size_t>(), 2, "query returns every record");
    checkEqual(all["nextAfter"].get<uint64_t>(), 2, "query cursor is the last visible id");
    check(all["hasMore"].get<bool>() == false, "a fully drained query reports hasMore=false");
    const auto& entries = all["entries"];
    checkEqual(entries[0]["status"].get<std::string>(), "ok", "success is normalized to ok");
    checkEqual(entries[0]["source"].get<std::string>(), "human", "source is kept verbatim");
    checkEqual(entries[0]["scope"].get<std::string>(), "scene", "scope is kept verbatim");
    checkEqual(entries[1]["summary"].get<std::string>(), "save path=scenes/my-scene.json",
               "a save summary names the path without dumping the document");
    check(entries[1]["detail"].contains("args"), "detail keeps the bounded request arguments");
    checkEqual(entries[1]["detail"]["method"].get<std::string>(), "save", "detail names the method");
    checkEqual(entries[1]["detail"]["scope"].get<std::string>(), "scene", "detail names the scope");

    const Json page = log.query(1, 100);
    checkEqual(page["count"].get<size_t>(), 1, "after=1 skips the first record");
    checkEqual(page["entries"][0]["id"].get<uint64_t>(), 2, "after is exclusive");
    const Json bounded = log.query(0, 1);
    checkEqual(bounded["count"].get<size_t>(), 1, "limit bounds the page");
    check(bounded["hasMore"].get<bool>(), "a truncated page reports hasMore=true");
    checkEqual(bounded["nextAfter"].get<uint64_t>(), 1, "the cursor advances to the last returned id");
    checkEqual(bounded["lastId"].get<uint64_t>(), 2, "lastId reports the newest id even on a bounded page");

    checkEqual(log.record("undo", "scene", "human", false, "undo failed", "History is empty"), 3, "failures are recorded");
    const Json failed = log.query(2, 100);
    checkEqual(failed["entries"][0]["status"].get<std::string>(), "error", "a rejected operation is an error");
    checkEqual(failed["entries"][0]["error"].get<std::string>(), "History is empty", "the error reason is kept");
}

void testQueryClassification() {
    check(ActivityLog::isQueryMethod("get_scene"), "get_scene is a polling query");
    check(ActivityLog::isQueryMethod("get_job"), "get_job is a polling query");
    check(ActivityLog::isQueryMethod("get_activity_log"), "get_activity_log is itself a query");
    check(!ActivityLog::isQueryMethod("apply"), "apply is a real operation");
    check(!ActivityLog::isQueryMethod("load_project"), "load_project is a real operation");
    check(!ActivityLog::isQueryMethod("export_animation"), "export is a real request");
    checkEqual(ActivityLog::normalizeStatus("success"), "ok", "success maps to ok");
    checkEqual(ActivityLog::normalizeStatus("failed"), "error", "failed maps to error");
    checkEqual(ActivityLog::normalizeStatus("cancelled"), "error", "cancelled is not silently treated as success");
}

void testCapacityIsBounded() {
    ActivityLog log;
    for (size_t i = 0; i < ActivityLog::kCapacity + 25; ++i)
        log.record("apply", "scene", "mcp", true, "create c" + std::to_string(i));
    checkEqual(log.size(), ActivityLog::kCapacity, "the log never exceeds its capacity");
    checkEqual(log.dropped(), 25, "dropped records are counted, not hidden");
    const Json page = log.query(0, ActivityLog::kCapacity);
    checkEqual(page["count"].get<size_t>(), ActivityLog::kCapacity, "a full page is available");
    checkEqual(page["entries"][0]["id"].get<uint64_t>(), 26, "the oldest records are evicted first");
    checkEqual(page["firstAvailableId"].get<uint64_t>(), 26, "firstAvailableId tracks eviction");
    checkEqual(log.lastId(), ActivityLog::kCapacity + 25, "ids keep increasing across eviction");
    checkEqual(log.firstAvailableId(), 26, "firstAvailableId is the oldest retained id");
}

void testTruncation() {
    std::cout << "  [4a] oversize summary/detail" << std::endl;
    ActivityLog log;
    const std::string huge(20000, 'x');
    const uint64_t id = log.record("replace", "scene", "mcp", true, huge,
                                   std::string(), Json{{"document", huge}});
    std::cout << "  [4b] query" << std::endl;
    const Json item = log.query(id - 1, 1)["entries"][0];
    const std::string summary = item["summary"].get<std::string>();
    check(summary.size() <= ActivityLog::kSummaryBytes + 3, "an oversize summary is truncated by bytes");
    check(summary.find("\u2026") != std::string::npos, "truncation is visible in the text");
    std::cout << "  [4c] omitted detail" << std::endl;
    check(item["detail"]["omitted"].get<bool>(), "an oversize detail is replaced by a bounded note");
    check(item["detail"]["bytes"].get<size_t>() > ActivityLog::kDetailBytes, "the omitted detail reports its size");
    check(item["detail"]["note"].get<std::string>().find("已省略") != std::string::npos, "the omission is explained in Chinese");
    std::cout << "  [4d] utf8 truncate" << std::endl;

    // 按字节截断不能劈开 UTF-8 码点：每个 '汉' 是 3 字节。
    const std::string chinese = "\u6c49\u5b57\u6d4b\u8bd5"; // 汉字测试
    std::string repeated;
    for (int i = 0; i < 200; ++i) repeated += chinese;
    const std::string cut = ActivityLog::truncate(repeated, 40);
    check(validUtf8(cut), "truncation never splits a UTF-8 code point");
    check(cut.size() <= 43, "the UTF-8 safe truncation still respects the byte budget");
    check(ActivityLog::truncate("short", 40) == "short", "short text is returned unchanged");
    std::cout << "  [4e] done" << std::endl;
}

void testCoalescing() {
    ActivityLog log;
    const std::string stamp = "2026-01-02T03:04:05.000Z";
    const uint64_t first = log.recordDetailed("playback", "scene", "human", "ok", "时间 / s 连续调整", std::string(),
                                              Json{{"slider", "time"}}, stamp, 1000);
    const uint64_t repeat = log.recordDetailed("playback", "scene", "human", "ok", "时间 / s 连续调整", std::string(),
                                               Json{{"slider", "time"}}, "2026-01-02T03:04:05.400Z", 1400);
    checkEqual(repeat, first, "a repeat inside the window reuses the same record");
    checkEqual(log.size(), 1, "a repeat does not add a row");
    checkEqual(log.items().back().repeats, 2, "the repeat counter is incremented");
    // 连击判定必须比较详情内容，不能只比字节数：下面两个 detail 长度完全相同，
    // 但作用对象不同，绝不能合并成一条（否则日志会篡改历史）。
    const uint64_t otherTarget = log.recordDetailed("playback", "scene", "human", "ok", "时间 / s 连续调整",
                                                    std::string(), Json{{"slider", "time2"}}, "2026-01-02T03:04:05.600Z", 1600);
    check(otherTarget != first, "same-length but different details are not coalesced");
    checkEqual(log.size(), 2, "a different detail adds its own row");
    checkEqual(log.items().back().detail["slider"].get<std::string>(), "time2", "the second target is kept");
    // 长度相同但键值互换：仍然必须区分。
    const uint64_t swapped = log.recordDetailed("playback", "scene", "human", "ok", "时间 / s 连续调整",
                                                std::string(), Json{{"a", 1}, {"b", 2}}, "2026-01-02T03:04:05.700Z", 1700);
    check(swapped != otherTarget, "a same-length detail with other values is not coalesced");
    checkEqual(log.size(), 3, "the swapped detail adds its own row");
    const uint64_t later = log.recordDetailed("playback", "scene", "human", "ok", "时间 / s 连续调整", std::string(),
                                              Json{{"slider", "time"}}, "2026-01-02T03:04:09.000Z", 5000);
    check(later != first, "a repeat outside the window is a new record");
    checkEqual(log.size(), 4, "the out-of-window repeat adds a row");
    // 失败不能被合并：那是需要人看见的事件。
    const uint64_t failure = log.record("playback", "scene", "human", false, "时间 / s 连续调整",
                                        "time outside scene duration", Json{{"slider", "time"}});
    check(failure != later, "a failure is never coalesced into a success");
    checkEqual(log.size(), 5, "the failure adds its own row");
    // warning 也不能与成功合并。
    const uint64_t warning = log.recordDetailed("save", "scene", "mcp", "warning", "磁盘日志写入失败", "磁盘日志写入失败",
                                                Json{{"path", "scenes/a.json"}}, "2026-01-02T03:04:09.200Z", 5200);
    check(warning != failure, "a warning is its own record");
    checkEqual(log.size(), 6, "the warning adds a row");
    checkEqual(log.items().back().status, "warning", "the warning status is preserved");
}

// 体积上限之外的详情会被替换成说明对象。两条**不同**的巨型详情在那之后无法区分，
// 因此绝不能因为「看起来一样」就合并；同一个巨型请求重复也不会被误并成一次。
void testTruncatedDetailsAreNeverCoalesced() {
    ActivityLog log;
    const std::string big(9000, 'x');
    const std::string stamp = "2026-01-02T03:04:05.000Z";
    const uint64_t first = log.recordDetailed("replace", "scene", "mcp", "ok", "replace", std::string(),
                                              Json{{"document", big + "a"}}, stamp, 1000);
    const uint64_t same = log.recordDetailed("replace", "scene", "mcp", "ok", "replace", std::string(),
                                             Json{{"document", big + "a"}}, "2026-01-02T03:04:05.100Z", 1100);
    const uint64_t other = log.recordDetailed("replace", "scene", "mcp", "ok", "replace", std::string(),
                                             Json{{"document", big + "b"}}, "2026-01-02T03:04:05.200Z", 1200);
    check(same != first, "an omitted (oversize) detail is never coalesced, even for the same input");
    check(other != same, "two different oversize details stay separate rows");
    checkEqual(log.size(), 3, "every oversize record keeps its own row");
    check(log.items().front().detailTruncated, "the omission is flagged on the stored record");
    check(log.items().front().detail["bytes"].get<size_t>() > ActivityLog::kDetailBytes,
          "the omission reports a size beyond the detail budget");
}

// after/limit 的游标语义：连击合并改写旧记录、clear() 重置 id、容量上限淘汰记录，
// 三种情况都不能让客户端把「丢了一段」误读成「没有新事件」。
void testCursorSemantics() {
    ActivityLog log;
    const std::string stamp = "2026-01-02T03:04:05.000Z";
    const uint64_t slider = log.recordDetailed("playback", "scene", "human", "ok", "slider", std::string(),
                                               Json{{"slider", "time"}}, stamp, 1000);
    const Json fresh = log.query(0, 10);
    check(fresh["updated"].get<bool>() == false, "a fresh record is not an update");
    checkEqual(fresh["missed"].get<uint64_t>(), 0, "nothing was dropped yet");
    checkEqual(fresh["epoch"].get<uint64_t>(), 0, "the first epoch is 0");
    check(fresh["reset"].get<bool>() == false, "a valid cursor is not a reset");

    // 连击合并：id 不前进，但 after 已经越过它的客户端必须看到「这条变了」。
    log.recordDetailed("playback", "scene", "human", "ok", "slider", std::string(), Json{{"slider", "time"}},
                       "2026-01-02T03:04:05.500Z", 1500);
    const Json merged = log.query(slider, 10);
    checkEqual(merged["count"].get<size_t>(), 0, "a merged repeat adds no new entry");
    check(merged["updated"].get<bool>(), "a merged repeat is reported as an update");
    checkEqual(merged["updatedCount"].get<uint64_t>(), 1, "exactly the merged record is reported");
    checkEqual(merged["updatedIds"][0].get<uint64_t>(), slider, "the updated id is the merged record");
    checkEqual(merged["nextAfter"].get<uint64_t>(), slider, "the cursor must not move for an update");
    const Json reread = log.query(slider - 1, 1);
    checkEqual(reread["entries"][0]["repeats"].get<unsigned>(), 2, "re-reading the id shows the merged count");

    // 容量上限淘汰：游标之后被丢掉的记录必须计入 missed。
    ActivityLog full;
    for (size_t i = 0; i < ActivityLog::kCapacity + 10; ++i) full.record("apply", "scene", "mcp", true, "c" + std::to_string(i));
    const Json evicted = full.query(5, 10);
    checkEqual(evicted["missed"].get<uint64_t>(), 5, "dropped records after the cursor are counted");
    checkEqual(evicted["firstAvailableId"].get<uint64_t>(), 11, "firstAvailableId tracks eviction");
    checkEqual(evicted["entries"][0]["id"].get<uint64_t>(), 11, "the page starts at the oldest retained record");
    check(evicted["updated"].get<bool>() == false, "eviction is not an update");
    const Json noGap = full.query(ActivityLog::kCapacity + 5, 10);
    checkEqual(noGap["missed"].get<uint64_t>(), 0, "a cursor inside the retained window misses nothing");

    // clear()：id 从头开始。旧游标必须被明确判定为失效（reset），而不是静默返回空页。
    ActivityLog cleared;
    for (int i = 0; i < 4; ++i) cleared.record("apply", "scene", "human", true, "a" + std::to_string(i));
    const uint64_t oldCursor = cleared.lastId();
    cleared.clear();
    const Json afterClear = cleared.query(oldCursor, 10);
    check(afterClear["reset"].get<bool>(), "a cursor from before clear() is reported as reset");
    checkEqual(afterClear["count"].get<size_t>(), 0, "a cleared log has nothing to return");
    checkEqual(afterClear["nextAfter"].get<uint64_t>(), 0, "reset points the client back at 0");
    checkEqual(afterClear["epoch"].get<uint64_t>(), 1, "clear() advances the epoch");
    check(cleared.record("apply", "scene", "human", true, "fresh") == 1, "ids restart after clear");
    const Json restarted = cleared.query(oldCursor, 10);
    check(restarted["reset"].get<bool>(), "the stale cursor is still reported as reset");
    checkEqual(restarted["count"].get<size_t>(), 1, "the reset page carries the new session's records");
    checkEqual(restarted["nextAfter"].get<uint64_t>(), 1, "the reset page hands out a usable cursor");
    const Json fromZero = cleared.query(0, 10);
    check(fromZero["reset"].get<bool>() == false, "after=0 is always valid");
    checkEqual(fromZero["epoch"].get<uint64_t>(), 1, "the epoch travels with every page");
}

// 后台任务终态上报的两个纯函数：终态判定与去重键。
void testJobReportContract() {
    check(ActivityLog::isTerminalJobStatus("completed"), "completed is terminal");
    check(ActivityLog::isTerminalJobStatus("failed"), "failed is terminal");
    check(ActivityLog::isTerminalJobStatus("cancelled"), "cancelled is terminal");
    check(!ActivityLog::isTerminalJobStatus("idle"), "idle is not terminal");
    check(!ActivityLog::isTerminalJobStatus("running"), "running is not terminal");
    checkEqual(ActivityLog::jobReportKey("job-3", "completed"), "job-3", "the job id is the dedupe key");
    checkEqual(ActivityLog::jobReportKey("", "failed"), "failed", "a missing id falls back to the status");
    // 终态记录独立追加：完成/取消/失败各自一行，且保留来源与范围。
    ActivityLog log;
    log.recordRequest({{"method", "export_animation"}, {"args", {{"output", "exports/a.mp4"}}}}, "mcp", true,
                      std::string(), std::string(), "scene");
    const uint64_t cancelled = log.recordJob("export", "job-9", "scene", "mcp", "cancelled", std::string(),
                                             "export 任务已取消", 0);
    checkEqual(log.size(), 2, "a terminal record is appended next to the request");
    const Json page = log.query(0, 10);
    checkEqual(page["entries"][1]["method"].get<std::string>(), "export.job", "the terminal record has its own method");
    checkEqual(page["entries"][1]["status"].get<std::string>(), "warning", "cancelled is a warning, not a success");
    checkEqual(page["entries"][1]["source"].get<std::string>(), "mcp", "the terminal record keeps its source");
    checkEqual(page["entries"][1]["scope"].get<std::string>(), "scene", "the terminal record keeps its scope");
    checkEqual(page["entries"][1]["detail"]["status"].get<std::string>(), "cancelled", "the raw worker status is kept");
    checkEqual(cancelled, page["entries"][1]["id"].get<uint64_t>(), "the appended record gets its own id");
}

void testSourceScopeAndJobOutcome() {
    ActivityLog log;
    const uint64_t started = log.recordRequest({{"method", "export_animation"}, {"args", {{"output", "exports/a.mp4"}}}},
                                               "mcp", true, std::string(), std::string(), "component");
    const Json item = log.query(0, 1)["entries"][0];
    checkEqual(item["scope"].get<std::string>(), "component", "an explicit scope override wins over the request");
    checkEqual(item["source"].get<std::string>(), "mcp", "an MCP request is attributed to MCP");

    const uint64_t updated = log.recordJob("export", "job-7", "component", "mcp", "failed", "ffmpeg missing",
                                           std::string(), started);
    checkEqual(updated, started, "a job terminal state updates its own start record");
    checkEqual(log.size(), 1, "updating a job outcome does not add a row");
    const Json after = log.query(0, 10)["entries"][0];
    checkEqual(after["status"].get<std::string>(), "error", "a failed job becomes error");
    checkEqual(after["error"].get<std::string>(), "ffmpeg missing", "the job error is visible");
    checkEqual(after["detail"]["status"].get<std::string>(), "failed", "the raw worker status stays in detail");
    checkEqual(after["summary"].get<std::string>(), "export 任务 失败", "a failed job without a summary gets a default label");

    const uint64_t orphan = log.recordJob("formula", "job-8", "scene", "human", "completed", std::string(),
                                          "公式已添加 · 修订 12", 999); // 起始记录已被淘汰
    check(orphan != 0 && log.size() == 2, "an unseen start id appends a standalone job record");
    const Json orphanItem = log.query(0, 10)["entries"][1];
    checkEqual(orphanItem["status"].get<std::string>(), "ok", "a completed job is ok");
    checkEqual(orphanItem["method"].get<std::string>(), "formula.job", "a job record has its own method name");
}

void testColumnsAlwaysKeepTheCanvas() {
    const float widths[] = {320.f, 400.f, 480.f, 560.f, 640.f, 800.f, 960.f, 1440.f, 1920.f};
    for (const float width : widths) {
        for (const bool open : {false, true}) {
            const EditorColumns columns = computeEditorColumns(width, 320.f, 340.f, open);
            const std::string at = " at width " + std::to_string(static_cast<int>(width)) +
                                   (open ? " (open)" : " (collapsed)");
            // 唯一硬性保证：画布永远有位置，且三列加起来不出屏。
            check(columns.drawing >= 160.f - 0.01f, "the canvas keeps at least 160 px" + at);
            check(columns.sidebar >= 0.f && columns.activity >= 0.f && columns.drawing >= 0.f,
                  "columns are never negative" + at);
            const float used = columns.sidebar + columns.sidebarSplitter + columns.activity +
                               columns.activitySplitter;
            check(used + columns.drawing <= width + 0.01f, "the three columns never exceed the window" + at);
            // 侧栏要么可用，要么整栏隐藏：不允许留下一条无意义的窄缝。
            check(columns.sidebar == 0.f || columns.sidebar >= 105.f,
                  "the left column is either usable or hidden, never a sliver" + at);
            check(!columns.activityShown || columns.activity >= 24.f - 0.01f,
                  "a shown log column is at least a button bar" + at);
            // 日志栏只有完整显示时才占分隔条；按钮条不需要可拖动的分隔条。
            check(columns.activity <= 0.f || columns.activityBar || columns.activity >= 240.f - 0.01f,
                  "a fully shown log column keeps its 240 px minimum" + at);
            check((columns.activitySplitter > 0.f) == (columns.activityShown && !columns.activityBar),
                  "the log splitter exists exactly when the log column is fully shown" + at);
        }
    }
    // 默认窗口（1440）上两栏都按请求宽度显示，画布仍然最宽。
    const EditorColumns wide = computeEditorColumns(1440.f, 320.f, 340.f, true);
    checkNear(wide.sidebar, 320.f, "a wide window keeps the requested left column");
    checkNear(wide.activity, 340.f, "a wide window keeps the requested log column");
    check(wide.drawing > wide.activity && wide.drawing > wide.sidebar, "the canvas stays the widest column");
    check(wide.activityShown && !wide.activityBar, "the log column is fully shown when there is room");
    // 极窄窗口：日志整栏隐藏，左栏收缩，画布仍然存在。
    const EditorColumns tiny = computeEditorColumns(300.f, 320.f, 340.f, true);
    check(!tiny.activityShown && tiny.activity == 0.f, "a very narrow window hides the log column");
    check(tiny.drawing >= 160.f - 0.01f, "a very narrow window still keeps the canvas");
    // 窄窗口优先保主编辑功能：左栏（图元/属性）仍按请求宽度，先让位的是只读日志栏。
    const EditorColumns tight = computeEditorColumns(640.f, 320.f, 340.f, true);
    checkNear(tight.sidebar, 320.f, "a tight window keeps the requested editing column");
    check(tight.drawing >= 160.f - 0.01f, "the canvas survives a tight window");
    check(tight.activityBar && tight.activity >= 24.f,
          "under pressure the read-only log column degrades to its button bar first");
    checkNear(tight.activity, 24.f, "the degraded log column is exactly the button bar");
    // 再窄一点：左栏才让步，但仍然可用；日志栏保持按钮条（它是重新展开的入口）。
    const EditorColumns narrower = computeEditorColumns(480.f, 320.f, 340.f, true);
    check(narrower.sidebar >= 140.f && narrower.sidebar < 320.f,
          "only after the log column gave way does the editing column shrink");
    check(narrower.activityBar && narrower.activity >= 24.f, "the log column keeps its button bar");
    check(narrower.drawing >= 160.f - 0.01f, "the canvas still survives");
    // 有空间但放不下请求宽度时，日志栏退到下限宽度，而不是直接变成按钮条。
    const EditorColumns midsize = computeEditorColumns(800.f, 320.f, 340.f, true);
    checkNear(midsize.activity, 240.f, "a midsize window keeps the log column at its minimum width");
    check(!midsize.activityBar && midsize.activityShown, "240 px is still a fully drawn log column");
    checkNear(midsize.sidebar, 320.f, "the editing column is untouched at midsize widths");
    // 折叠态：日志栏不占宽度（窗口够宽时只留展开按钮）。
    const EditorColumns collapsed = computeEditorColumns(1440.f, 320.f, 340.f, false);
    checkNear(collapsed.activity, 24.f, "a collapsed log column only keeps its button bar");
    checkNear(collapsed.sidebar, 320.f, "collapsing the log column does not touch the editing column");
}

// 「编辑画布」宽度是文字/公式标称像素基准的拟合宽度：它必须与只读日志栏的
// 展开/收起/拖动完全无关，否则同一个文档会因为侧栏布局导出不同的像素。
void testEditingCanvasIgnoresTheLogColumn() {
    for (const float width : {640.f, 900.f, 1440.f, 1920.f}) {
        for (const float requested : {240.f, 340.f, 700.f}) {
            const EditorColumns open = computeEditorColumns(width, 320.f, requested, true);
            const EditorColumns closed = computeEditorColumns(width, 320.f, requested, false);
            const std::string at = " at width " + std::to_string(static_cast<int>(width)) +
                                   " with log width " + std::to_string(static_cast<int>(requested));
            checkNear(open.editingDrawing, closed.editingDrawing,
                      "the editing canvas width does not depend on the log column" + at);
            // 左栏按请求宽度，只扣一个分隔条与画布空隙。
            checkNear(open.editingDrawing, width - 320.f - 6.f - 12.f,
                      "the editing canvas is the canvas+editing column geometry" + at);
            check(open.editingDrawing >= open.drawing - 0.01f,
                  "the editing canvas is never narrower than the visible canvas" + at);
            // 基准宽度不随用户折叠/拖动日志栏变化，但画布本身可以。
            if (width >= 900.f) check(open.drawing < open.editingDrawing,
                                      "an open log column does take width away from the canvas" + at);
        }
    }
    // 折叠日志栏（只留按钮条）同样不参与基准：基准只看左栏与窗口宽度。
    const EditorColumns bar = computeEditorColumns(1440.f, 500.f, 340.f, false);
    checkNear(bar.editingDrawing, 1440.f - 500.f - 6.f - 12.f, "the bar form does not change the baseline");
}

void testClear() {
    ActivityLog log;
    log.record("apply", "scene", "human", true, "create ball");
    log.record("apply", "scene", "human", true, "move ball");
    log.clear();
    check(log.empty(), "clear empties the log");
    checkEqual(log.lastId(), 0, "clear resets the id sequence");
    checkEqual(log.query(0, 10)["count"].get<size_t>(), 0, "a cleared log answers with an empty page");
    checkEqual(log.record("apply", "scene", "human", true, "create ball"), 1, "ids restart after clear");
}
}

int main() {
    try {
        std::cout << "[1] record/query" << std::endl; testRecordAndQuery();
        std::cout << "[2] classification" << std::endl; testQueryClassification();
        std::cout << "[3] capacity" << std::endl; testCapacityIsBounded();
        std::cout << "[4] truncation" << std::endl; testTruncation();
        std::cout << "[5] coalescing" << std::endl; testCoalescing();
        std::cout << "[5b] truncated details" << std::endl; testTruncatedDetailsAreNeverCoalesced();
        std::cout << "[6] job outcome" << std::endl; testSourceScopeAndJobOutcome();
        std::cout << "[6b] job report contract" << std::endl; testJobReportContract();
        std::cout << "[7] columns" << std::endl; testColumnsAlwaysKeepTheCanvas();
        std::cout << "[7b] editing canvas baseline" << std::endl; testEditingCanvasIgnoresTheLogColumn();
        std::cout << "[8] clear/cursor" << std::endl; testClear();
        std::cout << "[9] cursor semantics" << std::endl; testCursorSemantics();
    } catch (const std::exception& error) {
        std::cout << "EXCEPTION: " << error.what() << '\n';
        return 2;
    }
    if (failures) { std::cout << failures << " check(s) failed\n"; return 1; }
    std::cout << "activity log: all checks passed\n";
    return 0;
}
