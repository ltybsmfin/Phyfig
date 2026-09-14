"""Explicit --stdio transport: JSON lines, MCP-visible presentation, bounded failure.

This test always starts its own dedicated editor process; it never attaches to a
user's running editor and never creates a named pipe.
"""
import os
import subprocess
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp"))

from bridge import StdioEditorBridge  # noqa: E402


def editor_env():
    return {**os.environ, "VK_LAYER_VALIDATE_SYNC": "1"}


def find_editor_window(pid, timeout=15.0):
    """Return the editor's top-level window for one specific child pid."""
    import win32gui
    import win32process

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = []

        def collect(handle, _):
            if win32process.GetWindowThreadProcessId(handle)[1] == pid and \
                    win32gui.GetClassName(handle) == "VulkanKinematicsLesson":
                found.append(handle)

        win32gui.EnumWindows(collect, None)
        if found:
            return found[0]
        time.sleep(0.05)
    raise AssertionError(f"No editor window found for pid {pid}")


def verify(executable: Path):
    import win32con
    import win32gui

    workspace = ROOT / "build" / ("stdio-workspace-" + uuid.uuid4().hex)
    workspace.mkdir(parents=True, exist_ok=True)
    log_path = ROOT / "build/mcp-stdio-test.log"
    with log_path.open("w", encoding="utf-8") as log:

        # Happy path: get_scene/apply/undo/redo, source=mcp shared history,
        # a real presented frame, malformed JSON kept non-fatal.
        with StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=log) as bridge:
            scene = bridge.request("get_scene")
            assert scene["revision"] == 0
            initial = scene["document"]
            scene = bridge.request("apply", {"commands": [
                {"op": "create", "id": "stdio-note", "type": "text", "name": "stdio note",
                 "properties": {"x": 4, "y": 2.4, "text": "stdio transport"}},
                {"op": "move", "id": "title", "dx": 0, "dy": 0.4}]}, revision=0)
            assert scene["revision"] == 1, scene["revision"]
            view = bridge.request("get_view_state")
            assert view["visible"] and not view["minimized"] and view["presentedFrames"] > 0, view
            # The presented revision only advances when the next frame is drawn,
            # so wait for it in bounded time instead of racing the assertion.
            deadline = time.monotonic() + 5.0
            while view["presentedRevision"] != scene["revision"] and time.monotonic() < deadline:
                time.sleep(0.05)
                view = bridge.request("get_view_state")
            assert view["presentedRevision"] == scene["revision"], view
            history = bridge.request("get_history")
            assert history["entries"] and all(entry["source"] == "mcp" for entry in history["entries"]), history
            journal = Path(history["journalPath"])
            assert journal.is_file() and journal.is_relative_to(workspace), journal
            assert not history["journalError"], history
            bad = bridge.exchange_raw(b"{not json}\n")
            assert bad["ok"] is False and "Invalid JSON" in bad["error"], bad
            assert bridge.request("get_scene")["revision"] == 1
            before = bridge.request("get_scene")
            try:
                bridge.request("apply", {"commands": [{"op": "delete", "id": "missing"}]}, revision=1)
                raise AssertionError("invalid batch was accepted")
            except ValueError:
                pass
            assert bridge.request("get_scene") == before
            scene = bridge.request("undo", revision=1)
            assert scene["revision"] == 2 and scene["document"] == initial
            assert bridge.request("redo", revision=2)["revision"] == 3
            # Saving a project clears projectDirty, so the EOF below is a clean safe end.
            project = bridge.request("get_project")
            bridge.request("save_project", {"path": str(workspace / "session.physproj"),
                                            "expectedToken": project["token"], "overwrite": True})
        assert bridge.process.returncode == 0, f"Editor exit {bridge.process.returncode}"

        # Oversize input is rejected in bounded time and ends the transport.
        with StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=log) as oversized:
            payload = b'{"method":"get_scene","args":{},"pad":"' + b"x" * (1024 * 1024 + 64) + b'"}\n'
            response = oversized.exchange_raw(payload, timeout=20)
            assert response["ok"] is False and "1 MB" in response["error"], response

        # Wire FIFO: a valid request, an empty line and malformed JSON are answered
        # in arrival order, and a protocol error never kills the connection.
        with StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=log) as fifo:
            first = fifo.exchange_raw(b'{"method":"get_scene","args":{},"scope":"scene"}\n')
            assert first.get("ok") is True and "revision" in first["result"], first
            blank = fifo.exchange_raw(b"\n")
            assert blank.get("ok") is False and blank["error"] == "Empty request line", blank
            broken = fifo.exchange_raw(b"{not json}\n")
            assert broken.get("ok") is False and "Invalid JSON" in broken["error"], broken
            last = fifo.exchange_raw(b'{"method":"get_scene","args":{},"scope":"scene"}\n')
            assert last.get("ok") is True and "revision" in last["result"], last

        # EOF on stdin ends a clean editor without any further request.
        eof = StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=log)
        try:
            eof.request("get_scene")
            eof.process.stdin.close()
            assert eof.process.wait(timeout=15) == 0, eof.process.returncode
        finally:
            eof.close()

        # Consecutive requests followed immediately by EOF: every response already
        # accepted must be flushed, in order, before the editor exits. This is the
        # regression guard for shutdown cancelling the writer with output queued.
        drain = StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=log)
        try:
            count = 8
            line = b'{"method":"get_scene","args":{},"scope":"scene"}\n'
            drain.send_raw(line * count)
            drain.process.stdin.close()
            responses = [drain.next_response(timeout=20) for _ in range(count)]
            assert all(r.get("ok") is True for r in responses), responses
            revisions = [r["result"]["revision"] for r in responses]
            assert len(set(revisions)) == 1, revisions
            assert drain.process.wait(timeout=15) == 0, drain.process.returncode
        finally:
            drain.close()

        # A connected client plus a real window close must shut down in bounded
        # time: the reader is parked in a blocking ReadFile, so shutdown has to
        # cancel that synchronous I/O instead of joining forever.
        closed = StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=log)
        try:
            closed.request("get_scene")
            window = find_editor_window(closed.process.pid)
            win32gui.PostMessage(window, win32con.WM_CLOSE, 0, 0)
            assert closed.process.wait(timeout=15) == 0, closed.process.returncode
        finally:
            closed.close()

        # Backpressure window close: this client never reads stdout, so the
        # editor's writer ends up blocked in WriteFile on a full pipe. Closing
        # the window must still end the process in bounded time by cancelling
        # that blocked write instead of joining it forever.
        busy = subprocess.Popen(
            [str(executable), "--stdio"], cwd=str(workspace), env=editor_env(),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            line = b'{"method":"get_scene","args":{},"scope":"scene"}\n'
            busy.stdin.write(line * 256)
            busy.stdin.flush()
            window = find_editor_window(busy.pid)
            time.sleep(2.0)  # let the unread stdout pipe fill while the editor runs
            win32gui.PostMessage(window, win32con.WM_CLOSE, 0, 0)
            assert busy.wait(timeout=15) == 0, busy.returncode
        finally:
            try:
                busy.stdin.close()
            except OSError:
                pass
            if busy.poll() is None:
                busy.kill()
            busy.wait(timeout=5)
            busy.stdout.close()

        # A silent peer times out; the connection is marked unusable, never reused.
        slow = StdioEditorBridge(sys.executable, stdio_arg=None, env=editor_env(), timeout=1.0,
                                 args=["-c", "import sys, time; sys.stdin.readline(); time.sleep(30)"])
        try:
            try:
                slow.request("get_scene")
                raise AssertionError("a silent child did not time out")
            except TimeoutError:
                pass
            try:
                slow.request("get_scene")
                raise AssertionError("a timed-out connection was reused")
            except RuntimeError as exc:
                assert "unusable" in str(exc), exc
        finally:
            slow.close()

    text = log_path.read_text(encoding="utf-8")
    assert "Validation errors: 0; warnings: 0" in text, text
    print("PASS: explicit --stdio JSON-lines transport; source=mcp shared history; presented revisions; "
          "FIFO-ordered normal/empty/bad-JSON lines; queued responses drained across EOF; malformed/oversize "
          "bounded rejection; backpressure window close and timeout cleanup")


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else \
        ROOT / "build/vs2022/bin/editor/Debug/physics_editor.exe"
    verify(executable)
