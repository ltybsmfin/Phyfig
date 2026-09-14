"""End-to-end: official MCP client -> stdio SDK server -> live native editor."""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]


async def verify(executable: Path):
    pipe = "physics-test-" + uuid.uuid4().hex
    log_path = ROOT / "build/mcp-editor-test.log"
    with log_path.open("w", encoding="utf-8") as log:
        editor = subprocess.Popen([str(executable), "--validation", "--pipe", pipe], cwd=ROOT,
                                  stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW,
                                  env={**os.environ, "VK_LAYER_VALIDATE_SYNC": "1"})
        try:
            server = StdioServerParameters(command=sys.executable,
                args=[str(ROOT / "mcp/server.py"), "--pipe", pipe, "--workspace", str(ROOT)])
            async with stdio_client(server) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    listed = await session.list_tools()
                    names = {tool.name for tool in listed.tools}
                    assert {"get_scene", "get_catalog", "apply_commands", "undo", "redo", "save_scene", "load_scene", "set_playback", "replace_scene"} <= names
                    assert {"show_editor", "get_view_state", "get_history"} <= names
                    assert {"set_formula", "export_animation", "get_job", "cancel_job"} <= names

                    async def call(name, args=None, error=False):
                        result = await session.call_tool(name, args or {})
                        assert result.isError == error, (name, result)
                        if error:
                            return result
                        return json.loads(next(c.text for c in result.content if c.type == "text"))

                    catalog = await call("get_catalog")
                    assert len(catalog["primitives"]) == 11
                    formula_names = catalog["formulas"]["assets"]
                    assert len(formula_names) == len(set(formula_names))
                    assert set(formula_names) == set(catalog["formulaLatex"])
                    scene = await call("get_scene")
                    initial = scene["document"]
                    assert (await call("get_job", {"kind": "formula"}))["status"] == "idle"
                    await call("set_formula", {"id": "invalid-formula", "latex": r"\input{a}", "expected_revision": scene["revision"]})
                    for _ in range(100):
                        job = await call("get_job", {"kind": "formula"})
                        if job["status"] != "running": break
                        await asyncio.sleep(.05)
                    assert job["status"] == "failed", job
                    assert (await call("get_scene"))["revision"] == scene["revision"]
                    scene = await call("apply_commands", {"expected_revision": scene["revision"], "commands": [
                        {"op": "create", "id": "mcp-note", "type": "text", "name": "MCP 创建的注释", "properties": {"x": 4, "y": 2.4, "text": "MCP 已连接 · 与鼠标共用撤销记录"}},
                        {"op": "move", "id": "title", "dx": 0, "dy": 0.4},
                        {"op": "create", "id": "glyph-probe", "type": "text", "properties": {"x": 1, "y": 1, "text": "缺字探针 😀"}},
                    ]})
                    assert scene["revision"] == 1
                    rendered = await call("get_view_state")
                    assert rendered["visible"] and not rendered["minimized"] and rendered["presentedFrames"] > 0
                    assert rendered["presentedRevision"] == scene["revision"], rendered
                    # A1: presented-frame viewport metadata is explicit and distinct
                    # from the latest drawn canvas; missing glyphs are reported with
                    # their full code point (the emoji is non-BMP).
                    assert rendered["canvasReady"] is True and rendered["canvasWidth"] > 0 and rendered["canvasHeight"] > 0, rendered
                    assert rendered["viewportSource"] == "presented" and rendered["scope"] == "scene", rendered
                    assert rendered["presented"]["source"] == "presented" and rendered["view"]["source"] == "live", rendered
                    assert len(rendered["origin"]) == 2 and len(rendered["pan"]) == 2 and rendered["zoom"] > 0, rendered
                    assert any(w["codepoint"] == 0x1F600 for w in rendered["textWarnings"]), rendered["textWarnings"]
                    await call("apply_commands", {"expected_revision": 0, "commands": [{"op": "delete", "id": "mcp-note"}]}, error=True)
                    before = await call("get_scene")
                    await call("apply_commands", {"expected_revision": 1, "commands": [
                        {"op": "move", "id": "ball", "dx": 20, "dy": 0}, {"op": "delete", "id": "missing"}]}, error=True)
                    assert await call("get_scene") == before
                    await call("apply_commands", {"expected_revision": 1, "commands": [
                        {"op": "update", "id": "position", "patch": {"properties": {"formula": "does-not-exist"}}}]}, error=True)
                    assert await call("get_scene") == before
                    scene = await call("undo", {"expected_revision": 1})
                    assert scene["document"] == initial
                    scene = await call("redo", {"expected_revision": scene["revision"]})
                    assert len(scene["document"]["objects"]) == len(initial["objects"]) + 2
                    scene = await call("set_playback", {"time": 15 / 9.8, "step": 3, "playing": False})
                    assert abs(scene["playback"]["time"] - 15 / 9.8) < 1e-9
                    await call("save_scene", {"path": "build/mcp-往返.json", "expected_revision": scene["revision"], "overwrite": True})
                    await call("save_scene", {"path": "../outside.json", "expected_revision": scene["revision"]}, error=True)
                    revised = await call("apply_commands", {"expected_revision": scene["revision"], "commands": [{"op": "delete", "id": "mcp-note"}]})
                    loaded = await call("load_scene", {"path": "build/mcp-往返.json", "expected_revision": revised["revision"]})
                    assert loaded["document"] == scene["document"]
                    history = await call("get_history")
                    events = history["entries"]
                    assert all(e["source"] == "mcp" for e in events)
                    assert any(e["status"] == "rejected" for e in events)
                    original = next(e for e in events if e["afterRevision"] == 1 and e["status"] == "success")
                    undone = next(e for e in events if e["request"]["method"] == "undo" and e["status"] == "success")
                    assert undone["relatedEvent"] == original["id"]
                    assert Path(history["journalPath"]).is_file() and not history["journalError"]
                    page = await call("get_history", {"after": original["id"], "limit": 2})
                    assert len(page["entries"]) == 2 and page["hasMore"]
                    # Minimized windows must still serve IPC without a UI deadlock.
                    import win32gui, win32process, win32con
                    windows = []
                    win32gui.EnumWindows(lambda h, _: windows.append(h) if win32process.GetWindowThreadProcessId(h)[1] == editor.pid else None, None)
                    assert windows
                    window = next(h for h in windows if win32gui.GetClassName(h) == "VulkanKinematicsLesson")
                    win32gui.ShowWindow(window, win32con.SW_MINIMIZE)
                    await call("get_scene")
                    frames_before = (await call("get_view_state"))["presentedFrames"]
                    await call("set_playback", {"time": 0.8, "playing": False})
                    restored = await call("get_view_state")
                    assert not win32gui.IsIconic(window) and win32gui.IsWindowVisible(window)
                    assert restored["presentedFrames"] > frames_before and restored["presentedRevision"] == loaded["revision"]
                    assert abs(restored["presentedTime"] - 0.8) < 1e-9
                    win32gui.ShowWindow(window, win32con.SW_MINIMIZE)
                    await call("show_editor")
                    assert not (await call("get_view_state"))["minimized"]
                    # The same log is flushed to disk before the process exits.
                    disk = [json.loads(line) for line in Path(history["journalPath"]).read_text(encoding="utf-8").splitlines()]
                    assert disk[0]["kind"] == "session" and disk[1]["source"] == "mcp"
                    # Parameterized problem asset, isolated draft and rendered history scrubbing.
                    scene = await call("load_scene", {"path": "scenes/incline-arc.json", "expected_revision": loaded["revision"]})
                    head_before = await call("get_history")
                    draft = await call("open_component", {"id": "incline_arc", "expected_revision": scene["revision"]})
                    assert draft["scope"] == "component" and draft["revision"] == 0
                    draft = await call("apply_commands", {"scope": "component", "expected_revision": 0, "commands": [
                        {"op": "component_settings", "patch": {"parameters": {"R": {"default": 0.4}}}},
                    ]})
                    assert (await call("get_scene"))["document"] == scene["document"]
                    assert await call("get_history") == head_before
                    rendered = await call("get_view_state")
                    assert rendered["presentedScope"] == "component" and rendered["presentedRevision"] == draft["revision"], rendered
                    draft_log = await call("get_history", {"scope": "component"})
                    assert draft_log["journalPath"] != head_before["journalPath"]
                    scene = await call("publish_component", {"expected_revision": scene["revision"], "component_revision": draft["revision"]})
                    assert (await call("get_history"))["lastId"] == head_before["lastId"] + 1
                    assert (await call("get_history", {"scope": "component"})) == draft_log
                    assert scene["document"]["components"]["incline_arc"]["componentDefinition"]["parameters"]["R"]["default"] == 0.4
                    await call("apply_commands", {"expected_revision": scene["revision"], "commands": [
                        {"op": "update", "id": "apparatus", "patch": {"properties": {"parameters": {"R": -1}}}}
                    ]}, error=True)
                    scene = await call("apply_commands", {"expected_revision": scene["revision"], "commands": [
                        {"op": "update", "id": "apparatus", "patch": {"properties": {"parameters": {"L": 2.3, "angle": 45}}}},
                        {"op": "create", "id": "second", "type": "component", "properties": {"definition": "incline_arc", "x": 3, "rotation": 20, "scale": 0.7}},
                    ]})
                    history_before_preview = await call("get_history")
                    timeline = await call("get_timeline")
                    old_revision = head_before["entries"][-1]["afterRevision"]
                    preview = await call("preview_history", {"revision": old_revision})
                    assert preview["previewing"] and preview["headRevision"] == scene["revision"]
                    rendered = await call("get_view_state")
                    assert rendered["presentedScope"] == "scene" and rendered["presentedRevision"] == old_revision, rendered
                    assert (await call("get_scene"))["document"] == scene["document"]
                    assert await call("get_history") == history_before_preview
                    await call("undo", {"expected_revision": scene["revision"]}, error=True)
                    restored_scene = await call("restore_history", {"expected_revision": scene["revision"]})
                    assert not restored_scene["previewing"] and len(restored_scene["document"]["objects"]) == 1
                    scene_again = await call("undo", {"expected_revision": restored_scene["revision"]})
                    assert scene_again["document"] == scene["document"]
                    await call("preview_history", {"revision": 0, "scope": "component"})
                    assert not (await call("get_scene"))["previewing"]
                    assert (await call("get_scene", {"scope": "component"}))["previewing"]
                    await call("preview_history", {"scope": "component"})
                    await call("close_component", error=True)
                    await call("save_scene", {"scope": "component", "expected_revision": draft["revision"], "path": "build/component-roundtrip.json", "overwrite": True})
                    await call("close_component")
                    await call("get_scene", {"scope": "component"}, error=True)
                    await call("save_scene", {"expected_revision": scene_again["revision"], "path": "build/components-scene.json", "overwrite": True})
                    # Portable project saves scoped histories and can continue undo after reopen.
                    project = await call("get_project")
                    await call("save_project", {"path": "build/mcp-project.physproj", "expected_token": project["token"], "overwrite": True})
                    project = await call("get_project")
                    archive_scene = (await call("get_scene"))["document"]
                    await call("load_project", {"path": "build/mcp-project.physproj", "expected_token": project["token"]})
                    reopened = await call("get_scene")
                    assert reopened["document"] == archive_scene and reopened["canUndo"]
                    assert (await call("get_project"))["closedDraftCount"] >= 1
                    anim = {"channels": {"x": {"kind": "formula", "segments": [{"start": 0, "end": reopened["document"]["duration"], "expression": "t^2"}]}}}
                    await call("apply_commands", {"expected_revision": reopened["revision"], "commands": [{"op": "create", "id": "animated", "type": "point", "animation": anim}]})
                    assert (await call("sample_animation", {"id": "animated", "time": 0.5}))["state"]["x"] == 0.25
                    project = await call("get_project")
                    await call("save_project", {"path": "build/mcp-project.physproj", "expected_token": project["token"], "overwrite": True})
                    # Request normal close so validation includes resource destruction.
                    win32gui.PostMessage(window, win32con.WM_CLOSE, 0, 0)
            code = await asyncio.to_thread(editor.wait, 15)
            assert code == 0, f"Editor exit {code}; inspect {log_path}"
        finally:
            if editor.poll() is None:
                editor.terminate()
                editor.wait(timeout=5)
    text = log_path.read_text(encoding="utf-8")
    assert "Validation errors: 0; warnings: 0" in text, text
    print("PASS: MCP lifecycle/editing; atomicity; history sources/undo links/disk journal; actual presented revisions; auto-restore and visible rendering; clean Vulkan shutdown")


def verify_server_arguments():
    """Delta 2: stdio transport without --start-editor is rejected before any editor starts.

    The live stdio session (server -> private editor) is covered by
    test_runtime_assets.py, which has the longer budget this second editor needs.
    """
    result = subprocess.run(
        [sys.executable, str(ROOT / "mcp/server.py"), "--editor-transport", "stdio"],
        capture_output=True, text=True, timeout=30)
    assert result.returncode == 2 and "--start-editor" in result.stderr, (result.returncode, result.stderr)
    print("PASS: --editor-transport stdio requires --start-editor")


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/vs2022/bin/editor/Debug/physics_editor.exe"
    verify_server_arguments()
    asyncio.run(verify(executable))
