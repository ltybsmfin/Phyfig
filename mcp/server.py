"""Official SDK stdio server. stdout is reserved for MCP messages."""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP
from bridge import EditorBridge, scene_path

parser = argparse.ArgumentParser()
parser.add_argument("--pipe", default="physics-editor")
parser.add_argument("--workspace", type=Path, default=Path(__file__).resolve().parents[1])
parser.add_argument("--start-editor", type=Path)
options = parser.parse_args()
bridge = EditorBridge(options.pipe)
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
    return result


@mcp.tool()
def show_editor() -> dict[str, Any]:
    """Show/restore the real native editor window. Query get_view_state to observe presented frames."""
    return bridge.request("show_editor")


@mcp.tool()
def get_view_state() -> dict[str, Any]:
    """Read native window visibility, minimized state, and the revision/time of the last presented canvas frame."""
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


if __name__ == "__main__":
    if options.start_editor:
        if not bridge.available():
            subprocess.Popen([str(options.start_editor.resolve()), "--pipe", options.pipe], cwd=root,
                             stdin=subprocess.DEVNULL, stdout=sys.stderr, stderr=sys.stderr,
                             creationflags=subprocess.CREATE_NO_WINDOW)
            bridge.request("get_scene")
        bridge.request("show_editor")
    mcp.run(transport="stdio")
