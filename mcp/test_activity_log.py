"""右侧「操作日志」侧栏的 MCP 验收：真实编辑器 + --stdio 传输。

这个测试只通过现有的 dispatch 入口说话（stdio JSON-lines，与 MCP 服务器同一
条路径），因此它验证的正是「MCP 发起的操作会不会出现在操作日志里、轮询会不会
刷屏、失败是否可见、after/limit 是否有界」。

它刻意不启动 MCP SDK、不创建命名管道：它只管编辑器侧的只读查询入口
``get_activity_log``。mcp/server.py 是否把它暴露成 MCP 工具由根代理接线。

    python mcp/test_activity_log.py [physics_editor.exe] [--data-dir DIR]

前提：编辑器可以在本机开出窗口（与 mcp/test_stdio.py 相同）。
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp"))

from bridge import StdioEditorBridge  # noqa: E402


def editor_env() -> dict:
    return {**os.environ, "VK_LAYER_VALIDATE_SYNC": "1"}


def activity(bridge, after: int = 0, limit: int = 200, scope: str = "scene") -> dict:
    return bridge.request("get_activity_log", {"after": after, "limit": limit}, scope=scope)


def raw_request(bridge, method: str, args: dict, revision: int | None = None,
                scope: str = "scene") -> dict:
    """Send one request and return the raw {ok,result|error} envelope."""
    request = {"method": method, "args": args, "scope": scope}
    if revision is not None:
        request["expectedRevision"] = revision
    return bridge.exchange_raw((json.dumps(request, ensure_ascii=False) + "\n").encode("utf-8"))


def entry_ids(log: dict) -> list[int]:
    return [entry["id"] for entry in log["entries"]]


def find_entry(log: dict, method: str) -> dict | None:
    for entry in log["entries"]:
        if entry["method"] == method:
            return entry
    return None


def last_entry(log: dict, method: str) -> dict | None:
    for entry in reversed(log["entries"]):
        if entry["method"] == method:
            return entry
    return None


def verify(executable: Path, data_dir: Path, workspace: Path) -> None:
    """一次完整的会话：写入、失败、查询、游标、连击可见性。"""
    # 子进程 stderr 落在本次会话的工作目录里（临时目录）：测试不往源码树的
    # build/ 里写文件，只读/受限的 checkout 也能跑。
    log_path = workspace / "activity-log-test.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as sink:
        with StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=sink,
                               args=["--data-dir", str(data_dir)]) as bridge:
            scene = bridge.request("get_scene")
            assert scene["revision"] == 0, scene

            # 1) 只读查询成功时必须静默：轮询不能把有界日志刷满。
            for _ in range(8):
                bridge.request("get_scene")
                bridge.request("get_catalog")
                bridge.request("get_view_state")
                bridge.request("get_job", {"kind": "formula"})
            polled = activity(bridge)
            assert polled["count"] == 0, polled
            assert polled["lastId"] == 0, polled
            # get_activity_log 自己也不能被记录（否则每次查询都自增一条）。
            assert find_entry(polled, "get_activity_log") is None, polled

            # 2) MCP 的真实操作必须出现，并带上范围、时间与摘要。
            scene = bridge.request("apply", {"commands": [
                {"op": "create", "id": "log-note", "type": "text", "name": "日志验收",
                 "properties": {"x": 4, "y": 2.4, "text": "activity log"}},
                {"op": "move", "id": "title", "dx": 0, "dy": 0.3}]}, revision=scene["revision"])
            applied = activity(bridge)
            item = find_entry(applied, "apply")
            assert item is not None, applied
            assert item["source"] == "mcp", item
            assert item["scope"] == "scene", item
            assert item["status"] == "ok" and item["error"] == "", item
            assert "create" in item["summary"] and "log-note" in item["summary"], item
            assert item["utc"].endswith("Z") and len(item["utc"]) >= 20, item
            assert item["query"] is False, item
            assert item["detail"]["method"] == "apply", item
            assert "args" in item["detail"], item

            # 3) 失败必须可见：被拒绝的批次 + 过期的 expectedRevision。
            before = bridge.request("get_scene")
            try:
                bridge.request("apply", {"commands": [{"op": "delete", "id": "missing-object"}]},
                               revision=before["revision"])
                raise AssertionError("invalid batch was accepted")
            except ValueError:
                pass
            try:
                bridge.request("apply", {"commands": [{"op": "delete", "id": "log-note"}]},
                               revision=before["revision"] - 1)
                raise AssertionError("stale revision was accepted")
            except ValueError:
                pass
            failed = activity(bridge)
            errors = [entry for entry in failed["entries"] if entry["status"] == "error"]
            assert len(errors) >= 2, failed
            assert all(entry["error"] for entry in errors), failed
            assert all(entry["method"] == "apply" for entry in errors), failed
            # 被拒绝的操作没有改变文档：失败记录不是历史事件。
            assert bridge.request("get_scene") == before
            # 操作日志与磁盘/会话 journal（get_history）是两套不同的记录：journal
            # 属于**文档**事务，被拒绝的请求也会以 status="rejected" 记进去（旧行为，
            # 本次不改）。所以正确的断言是「拒绝出现在 journal 里、且没有推进修订」，
            # 而不是新测试最初假设的「journal 只剩成功记录」。
            history = bridge.request("get_history")
            rejected = [entry for entry in history["entries"] if entry["status"] == "rejected"]
            assert len(rejected) >= 2, history
            assert all(entry["request"]["method"] == "apply" for entry in rejected), rejected
            assert all(entry["beforeRevision"] == entry["afterRevision"] for entry in rejected), rejected
            assert all(entry.get("error") for entry in rejected), rejected

            # 4) 失败以外的真实操作也要出现：播放、撤销、导出请求（无参数被拒绝）。
            bridge.request("playback", {"time": 0.5, "playing": False})
            after_playback = activity(bridge)
            assert find_entry(after_playback, "playback") is not None, after_playback
            for name, args, revision in (
                # 线上方法名就是 "save"/"playback"；MCP 工具 save_scene/set_playback
                # 只是 server.py 的包装，编辑器不认识 "save_scene"。
                ("save", {"path": "build/activity-log-acceptance.json", "overwrite": True}, before["revision"]),
                ("undo", {}, before["revision"]),
            ):
                response = raw_request(bridge, name, args, revision)
                assert response["ok"] is True, response
            richer = activity(bridge)
            assert find_entry(richer, "save") is not None, richer
            assert find_entry(richer, "undo") is not None, richer
            # 异步任务（导出）请求被拒绝时同样入日志：它没有提交任何文档修改。
            try:
                bridge.request("export_animation", {"output": "build/should-not-exist.mp4"}, revision=0)
                raise AssertionError("stale export request was accepted")
            except ValueError:
                pass
            assert find_entry(activity(bridge), "export_animation") is not None, activity(bridge)

            # 5) after/limit 有界且语义一致：分页不重复、不丢记录。
            full = activity(bridge, after=0, limit=500)
            assert full["count"] == len(full["entries"]) >= 4, full
            ids = entry_ids(full)
            assert ids == sorted(ids) and len(set(ids)) == len(ids), ids
            assert full["nextAfter"] == ids[-1], full
            assert full["hasMore"] is False, full
            page = activity(bridge, after=ids[0], limit=2)
            assert entry_ids(page) == ids[1:3], (page, ids)
            assert page["hasMore"] is True, page
            assert page["nextAfter"] == ids[2], page
            tail = activity(bridge, after=ids[-1])
            assert tail["count"] == 0 and tail["nextAfter"] == ids[-1], tail
            assert activity(bridge, limit=1)["count"] == 1, "limit=1 must bound the page"
            assert full["capacity"] == 500 and full["dropped"] == 0, full
            assert full["firstAvailableId"] == ids[0], full
            assert full["reset"] is False and full["missed"] == 0, full
            assert full["epoch"] == 0, full
            assert tail["updated"] is False and tail["updatedIds"] == [], tail

            # 5b) 连击合并会改写一条**已经发出去**的记录，它的 id 不会前进。客户端按
            # after 轮询时收不到新条目，因此必须能从 updated/updatedIds 知道要重读哪
            # 一条（否则「同一操作连续发生」在日志里永远不可见）。
            bridge.request("playback", {"time": 0.25, "playing": False})
            bridge.request("playback", {"time": 0.25, "playing": False})  # 2 s 内相同请求 → 合并
            repeated = last_entry(activity(bridge), "playback")
            assert repeated is not None and repeated["repeats"] >= 2, repeated
            merged = activity(bridge, after=repeated["id"])
            assert merged["count"] == 0, merged
            assert merged["updated"] is True and repeated["id"] in merged["updatedIds"], merged
            assert merged["updatedCount"] >= 1, merged
            reread = activity(bridge, after=repeated["id"] - 1, limit=1)
            assert reread["entries"][0]["repeats"] == repeated["repeats"], reread

            # 6) 侧栏状态同一条查询即可确认：默认可见、可跟随、筛选齐备。
            sidebar = full["sidebar"]
            assert sidebar["open"] is True and sidebar["visible"] is True, sidebar
            assert sidebar["collapsedBar"] is False and sidebar["follow"] is True, sidebar
            assert 240.0 <= sidebar["width"] <= 700.0, sidebar
            assert set(sidebar["filters"]) == {"ui", "mcp", "failed", "quiet"}, sidebar
            assert sidebar["independentOfHistory"] is True, sidebar

            # 7) 参数校验：limit/after 必须是有界整数，非法值不改变日志。
            before_bad = activity(bridge)
            for bad in ({"limit": 0}, {"limit": 501}, {"limit": -3}, {"after": -1}):
                response = raw_request(bridge, "get_activity_log", bad)
                assert response["ok"] is False, (bad, response)
            assert activity(bridge)["count"] == before_bad["count"], "a rejected query must not change the log"

            # 8) 操作日志与文档撤销历史互不影响：清空日志不会动历史，撤销也不清日志。
            history_before = bridge.request("get_history")["lastId"]
            project = bridge.request("get_project")
            bridge.request("save_project", {"path": str(workspace / "activity-log.physproj"),
                                            "expectedToken": project["token"], "overwrite": True})
            assert bridge.request("get_history")["lastId"] == history_before, "saving the project is not a document edit"
            final = activity(bridge)
            assert find_entry(final, "save_project") is not None, final
        assert bridge.process.returncode == 0, f"editor exit {bridge.process.returncode}"

    text = log_path.read_text(encoding="utf-8")
    assert "Validation errors: 0; warnings: 0" in text, text[-4000:]
    print("PASS: activity log via MCP dispatch — polling stays silent, real operations are recorded with "
          "source/scope/time/summary, failures are visible and the document journal records them as "
          "rejected without advancing the revision, after/limit are bounded with reset/missed/updated "
          "cursor semantics, sidebar state is reported")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", nargs="?", type=Path,
                        default=ROOT / "build/vs2022/bin/editor/Debug/physics_editor.exe")
    parser.add_argument("--data-dir", type=Path, default=None,
                        help="writable data directory for journals; defaults to a fresh temp directory")
    arguments = parser.parse_args()
    executable = arguments.executable.resolve()
    if not executable.is_file():
        print(f"Missing editor executable: {executable}", file=sys.stderr)
        return 2
    if arguments.data_dir is not None:
        data_dir = arguments.data_dir.resolve()
        data_dir.mkdir(parents=True, exist_ok=True)
        workspace = data_dir
    else:
        # 自带临时数据目录与工作目录：不污染仓库，也不依赖用户当前目录。
        temporary = Path(tempfile.mkdtemp(prefix="activity-log-"))
        data_dir = temporary / "data"
        workspace = temporary / "workspace"
        data_dir.mkdir()
        workspace.mkdir()
    try:
        verify(executable, data_dir, workspace)
    finally:
        if arguments.data_dir is None:
            import shutil
            shutil.rmtree(workspace.parent, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
