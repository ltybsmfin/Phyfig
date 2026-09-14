"""Official SDK stdio server. stdout is reserved for MCP messages."""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP
from bridge import EditorBridge, StdioEditorBridge, scene_path

parser = argparse.ArgumentParser()
parser.add_argument("--pipe", default="physics-editor")
parser.add_argument("--workspace", type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument("--start-editor", type=Path)
# Explicit transport selection. pipe (default) attaches to (or starts) a named-pipe
# editor; stdio always starts its own editor and speaks JSON lines on its
# stdin/stdout, so it has no pipe fallback and cannot attach to an existing one.
parser.add_argument("--editor-transport", choices=("pipe", "stdio"), default="pipe",
                    help="pipe (default): named-pipe editor; stdio: start a private editor with --stdio")
options = parser.parse_args()
bridge: Any = EditorBridge(options.pipe)
root = options.workspace.resolve()
mcp = FastMCP("Physics diagram editor", instructions=(
    "Operate the live C++ Vulkan editor. First inspect get_catalog and get_scene. "
    "Coordinates are meters with +y up. Group related commands into one atomic batch. "
    "Pass the most recently read document revision to every document edit. "
    "If a call times out, inspect the scene before retrying. Do not automatically overwrite user edits. "
    "The scope argument is explicit: scene (default) or component draft, independent of GUI focus. "
    "History preview is read-only; exit it or restore as a new edit before modifying the document. "
    "Steps reveal objects; playback time evaluates motion. No natural-language solver or collision simulation is implied."
))


@mcp.tool()
def get_catalog(scope: str = "scene") -> dict[str, Any]:
    """Discover primitive defaults, command arguments, motion/binding rules and formula assets."""
    result = bridge.request("get_catalog", scope=scope)
    result["formulas"] = bridge.request("get_formulas")
    result["commandArguments"] = {
        "create": {"id": "unique string", "type": "primitive type", "name": "optional label", "properties": "partial defaults", "motion": "optional vx/vy/ax/ay", "binding": "optional target/kind/scale", "step": "optional integer"},
        "update": {"id": "existing id", "patch": "merge patch; id/type immutable; null removes optional motion/binding"},
        "delete": {"id": "existing id; remove dependants in same batch"},
        "move": {"id": "existing id", "dx": "meters", "dy": "meters"},
        "reorder": {"id": "existing id", "index": "0-based drawing order"},
        "install_component": {"definition": "complete component definition JSON; publishes a snapshot"},
        "component_settings": {"patch": "merge patch for componentDefinition metadata; component scope only"},
        "settings": {"title": "optional", "duration": "seconds in (0,3600]", "steps": "integer in [1,100]"},
        "set_animation": {"id": "object id", "animation": "parameters, quantities, channels; null removes animation"},
        "set_keyframe": {"id": "object id", "channel": "existing key channel", "time": "seconds", "value": "numeric", "interpolation": "linear | step | smooth"},
        "delete_keyframe": {"id": "object id", "channel": "key channel", "time": "existing key time"},
    }
    result["components"] = {
        "definition": "A scene-shaped JSON with componentDefinition: {id, parameters, bindings, constraints}. Static child primitives; no nested components in v1.",
        "parameter": {"R": {"default": 0.3, "min": 0.05, "max": 10, "unit": "m"}},
        "binding": {"object": "track", "property": "radius", "expression": "R"},
        "textBinding": {"object": "label", "property": "text", "template": "R = {R} m"},
        "constraint": {"expression": "L-R*cos(angle)/sin(angle)", "message": "A must be above B; expression must be > 0"},
        "expressions": "Numbers, public parameter names, + - * / parentheses, sin/cos in DEGREES, sqrt, abs. No scripting or property references.",
        "instance": "type=component; properties definition, parameters overrides, x/y, rotation degrees, uniform scale. Text stays upright in screen pixels.",
        "sample": "scenes/incline-arc.json and assets/components/incline-arc.json",
    }
    result["formulaLatex"] = __import__("json").loads((Path(__file__).resolve().parents[1] / "assets/formulas/formulas.json").read_text(encoding="utf-8"))
    result["formulaLatex"].update(result["formulas"].get("latex", {}))
    result["animation"] = {
        "parameters": {"F0": 4, "omega": 2},
        "quantities": {"F": {"expression": "F0*sin(omega*t)", "unit": "N"}},
        "channels": {"dy": {"kind": "formula", "segments": [{"start": 0, "end": 6, "expression": "0.5*F"}]}},
        "rules": "sin/cos/tan use RADIANS; sind/cosd use degrees. t global seconds, u local segment seconds. Segments exactly cover duration, right-continuous. keys: [{time,value,interpolation}]. Animation replaces numeric properties; it cannot coexist with motion/binding. F(t) only affects channels explicitly mapped to F, no automatic physics integration."
    }
    return result


@mcp.tool()
def set_formula(id: str, latex: str, expected_revision: int, scope: str = "scene",
                x: float = 3, y: float = 5, python: str = "python") -> dict[str, Any]:
    """Compile restricted math LaTeX asynchronously, then create/update one formula as an undoable edit.
    Requires MiKTeX plus Python with Pillow/pypdfium2. Poll get_job(kind='formula').
    A concurrent document edit causes a conflict, never an overwrite. Saved projects embed the mask and source.
    """
    return bridge.request("set_formula", {"id": id, "latex": latex, "x": x, "y": y, "python": python},
                          revision=expected_revision, scope=scope)


@mcp.tool()
def export_animation(output: str, expected_revision: int, scope: str = "scene", format: str = "mp4",
                     width: int = 0, height: int = 0, view_mode: str = "current", fps: int = 24,
                     overwrite: bool = False, python: str = "python", ffmpeg: str = "ffmpeg") -> dict[str, Any]:
    """Export the target scope's frozen document/step and full duration in the background; poll get_job('export').

    view_mode='current' (default) replays that scope's last recorded canvas
    composition; width=height=0 matches the recorded canvas size, while explicit
    even dimensions letterbox it. view_mode='fit' is an independent viewport for
    the target scope and requires explicit even width/height. A missing recorded
    view is rejected with a message suggesting fit; another scope's layout is
    never borrowed. Explicit sizes must be even, 16..4096; canvas-match rounds
    odd recorded sizes down by one pixel (MP4/GIF yuv420p needs even dimensions)
    and rejects oversize instead of silently scaling to 4096. Frames are
    ceil(duration*fps), capped by rejecting >3600 rather than changing speed.
    """
    if view_mode not in ("current", "fit"):
        raise ValueError("view_mode must be 'current' or 'fit'")
    if (width == 0) != (height == 0):
        raise ValueError("width and height must be paired: both 0 to match the canvas, or both explicit")
    if view_mode == "fit" and width == 0:
        raise ValueError("view_mode='fit' requires explicit even width/height; width=height=0 only matches the current canvas")
    destination = (root / output).resolve()
    if not destination.is_relative_to(root):
        raise ValueError("Export destination must be inside the workspace")
    return bridge.request("export_animation", {"output": str(destination), "format": format, "width": width,
        "height": height, "view_mode": view_mode, "fps": fps, "overwrite": overwrite, "python": python, "ffmpeg": ffmpeg},
        revision=expected_revision, scope=scope)


@mcp.tool()
def get_job(kind: str = "formula") -> dict[str, Any]:
    """Read the latest formula/export job id, status, result or failure without changing document history."""
    return bridge.request("get_job", {"kind": kind})


@mcp.tool()
def cancel_job(kind: str, id: str) -> dict[str, Any]:
    """Cancel the matching owned background process tree. Re-read get_job before cancelling."""
    return bridge.request("cancel_job", {"kind": kind, "id": id})


@mcp.tool()
def sample_animation(id: str, time: float, scope: str = "scene") -> dict[str, Any]:
    """Read evaluated properties and state at time without seeking, editing, or adding history."""
    return bridge.request("sample_animation", {"id": id, "time": time}, scope=scope)


@mcp.tool()
def get_project() -> dict[str, Any]:
    """Read project path, layout, dirty flag and concurrency token covering both documents, resources, histories and drafts."""
    return bridge.request("get_project")


def project_path(path: str) -> str:
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root) or resolved.suffix.lower() != ".physproj":
        raise ValueError("Project must be a .physproj file inside the workspace")
    return str(resolved)


@mcp.tool()
def save_project(path: str, expected_token: dict[str, Any], overwrite: bool = False) -> dict[str, Any]:
    """Atomically pack both documents, scoped undo/redo, audit, closed drafts, animation, layout and formula masks. Use the latest get_project.token."""
    return bridge.request("save_project", {"path": project_path(path), "expectedToken": expected_token, "overwrite": overwrite})


@mcp.tool()
def load_project(path: str, expected_token: dict[str, Any], discard: bool = False) -> dict[str, Any]:
    """Validate the complete project before replacing the workspace. Loading pauses playback. Unsaved edits require explicit discard=true or saving first."""
    return bridge.request("load_project", {"path": project_path(path), "expectedToken": expected_token, "discard": discard})


@mcp.tool()
def show_editor() -> dict[str, Any]:
    """Show/restore the real native editor window. Query get_view_state to observe presented frames."""
    return bridge.request("show_editor")


@mcp.tool()
def get_view_state() -> dict[str, Any]:
    """Read native window visibility, minimized state, the last presented frame and the latest drawn canvas.

    Top-level canvasWidth/canvasHeight (physical pixels), zoom, referenceZoom,
    origin (canvas-local pixels, +y up), pan, scope, canvasReady and
    viewportSource describe the last *presented* frame; the nested `view` object
    describes the latest drawn canvas (source='live') and is never used to fake a
    presented value. `presented` repeats the presented viewport explicitly.
    textWarnings lists document text objects whose glyphs the current font is
    missing, with object id and full Unicode code point.
    """
    return bridge.request("get_view_state")


@mcp.tool()
def get_history(after: int = 0, limit: int = 100, scope: str = "scene") -> dict[str, Any]:
    """Read shared human/MCP command history (1..100 entries per page), including rejected attempts and undo/redo links. Source identifies input route, not authenticated identity. Full session JSONL stays on disk."""
    return bridge.request("get_history", {"after": after, "limit": limit}, scope=scope)


@mcp.tool()
def get_scene(scope: str = "scene") -> dict[str, Any]:
    """Read the authoritative document, revision, playback and undo/redo availability."""
    return bridge.request("get_scene", scope=scope)


@mcp.tool()
def apply_commands(commands: list[dict[str, Any]], expected_revision: int, scope: str = "scene") -> dict[str, Any]:
    """Atomically apply edits. Each command has op plus fields listed by get_catalog. One batch = one undo."""
    return bridge.request("apply", {"commands": commands}, expected_revision, scope)


@mcp.tool()
def replace_scene(document: dict[str, Any], expected_revision: int, scope: str = "scene") -> dict[str, Any]:
    """Replace the complete versioned document in one undoable operation. Preserve unknown extension metadata."""
    return bridge.request("replace", {"document": document}, expected_revision, scope)


@mcp.tool()
def undo(expected_revision: int, scope: str = "scene") -> dict[str, Any]:
    """Undo the last document transaction from either the UI or MCP."""
    return bridge.request("undo", revision=expected_revision, scope=scope)


@mcp.tool()
def redo(expected_revision: int, scope: str = "scene") -> dict[str, Any]:
    """Redo one document transaction."""
    return bridge.request("redo", revision=expected_revision, scope=scope)


@mcp.tool()
def set_playback(time: float | None = None, playing: bool | None = None,
                 rate: float | None = None, step: int | None = None, scope: str = "scene") -> dict[str, Any]:
    """Seek, play/pause, change speed or reveal step. Does not add document history. Time is seconds."""
    return bridge.request("playback", {k: v for k, v in locals().items() if v is not None and k != "scope"}, scope=scope)


@mcp.tool()
def save_scene(path: str, expected_revision: int, overwrite: bool = False, scope: str = "scene") -> dict[str, Any]:
    """Save UTF-8 scene JSON inside the workspace. Existing files need explicit overwrite=true."""
    return bridge.request("save", {"path": scene_path(root, path), "overwrite": overwrite}, expected_revision, scope)


@mcp.tool()
def load_scene(path: str, expected_revision: int, scope: str = "scene") -> dict[str, Any]:
    """Load scene JSON from the workspace; whole-scene validation and undo apply."""
    return bridge.request("load", {"path": scene_path(root, path)}, expected_revision, scope)


@mcp.tool()
def open_component(id: str, expected_revision: int, objects: list[str] | None = None) -> dict[str, Any]:
    """Open installed definition as an isolated draft, or create a new definition from selected static scene object IDs (empty = blank). Main scene stays unchanged. Close existing draft first. expected_revision refers to the main scene."""
    return bridge.request("open_component", {"id": id, "objects": objects or []}, expected_revision)


@mcp.tool()
def close_component(discard: bool = False) -> dict[str, Any]:
    """Close the component draft. Unsaved edits require explicit discard=true; save_scene in component scope first to retain them."""
    return bridge.request("close_component", {"discard": discard})


@mcp.tool()
def publish_component(expected_revision: int, component_revision: int) -> dict[str, Any]:
    """Publish current draft snapshot as ONE main-scene transaction, updating all instances of that definition. Internal edit history stays in the draft. Both revisions must match."""
    return bridge.request("publish_component", {"componentRevision": component_revision}, expected_revision)


@mcp.tool()
def get_timeline(scope: str = "scene") -> dict[str, Any]:
    """List retained historical document states (last 101). Revisions are edit history, independent of physical time in seconds."""
    return bridge.request("get_timeline", scope=scope)


@mcp.tool()
def preview_history(revision: int | None = None, scope: str = "scene") -> dict[str, Any]:
    """Render a retained revision read-only in the live window. None exits preview. Does not mutate the head, undo stack, or append audit events. get_scene.document remains the authoritative head; displayRevision identifies the visible state."""
    return bridge.request("preview", {"revision": revision}, scope=scope)


@mcp.tool()
def restore_history(expected_revision: int, scope: str = "scene") -> dict[str, Any]:
    """Restore the previewed document state as a new undoable transaction. Requires explicit preview first. expected_revision is the head revision, not the preview cursor."""
    return bridge.request("restore_preview", revision=expected_revision, scope=scope)


def main() -> int:
    """Select the transport, start at most one editor, and own only that child."""
    global bridge
    if options.editor_transport == "stdio" and options.start_editor is None:
        parser.error("--editor-transport stdio requires --start-editor PATH: "
                     "the stdio bridge starts its own editor and never attaches to a pipe")
    stdio_editor = None
    try:
        if options.editor_transport == "stdio":
            # StdioEditorBridge owns exactly one editor process; the named-pipe
            # branch below must stay out of this path so no second editor starts.
            stdio_editor = StdioEditorBridge(options.start_editor, cwd=root, stderr_sink=sys.stderr)
            bridge = stdio_editor
            bridge.request("get_scene")
            bridge.request("show_editor")
        elif options.start_editor is not None:
            if not bridge.available():
                subprocess.Popen([str(options.start_editor.resolve()), "--pipe", options.pipe], cwd=root,
                                 stdin=subprocess.DEVNULL, stdout=sys.stderr, stderr=sys.stderr,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
                bridge.request("get_scene")
            bridge.request("show_editor")
        mcp.run(transport="stdio")
    finally:
        # Close only the child this process started; an editor attached through a
        # named pipe belongs to its own launcher and is left running.
        if stdio_editor is not None:
            stdio_editor.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
