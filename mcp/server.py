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
    "Steps reveal objects; playback time evaluates motion. No natural-language solver or collision simulation is implied."
))


@mcp.tool()
def get_catalog() -> dict[str, Any]:
    """Discover primitive defaults, command arguments, motion/binding rules and formula assets."""
    result = bridge.request("get_catalog")
    result["formulas"] = bridge.request("get_formulas")
    result["commandArguments"] = {
        "create": {"id": "unique string", "type": "primitive type", "name": "optional label", "properties": "partial defaults", "motion": "optional vx/vy/ax/ay", "binding": "optional target/kind/scale", "step": "optional integer"},
        "update": {"id": "existing id", "patch": "merge patch; id/type immutable; null removes optional motion/binding"},
        "delete": {"id": "existing id; remove dependants in same batch"},
        "move": {"id": "existing id", "dx": "meters", "dy": "meters"},
        "reorder": {"id": "existing id", "index": "0-based drawing order"},
        "settings": {"title": "optional", "duration": "seconds in (0,3600]", "steps": "integer in [1,100]"},
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
def get_history(after: int = 0, limit: int = 100) -> dict[str, Any]:
    """Read shared human/MCP command history (1..100 entries per page), including rejected attempts and undo/redo links. Source identifies input route, not authenticated identity. Full session JSONL stays on disk."""
    return bridge.request("get_history", {"after": after, "limit": limit})


@mcp.tool()
def get_scene() -> dict[str, Any]:
    """Read the authoritative document, revision, playback and undo/redo availability."""
    return bridge.request("get_scene")


@mcp.tool()
def apply_commands(commands: list[dict[str, Any]], expected_revision: int) -> dict[str, Any]:
    """Atomically apply edits. Each command has op plus fields listed by get_catalog. One batch = one undo."""
    return bridge.request("apply", {"commands": commands}, expected_revision)


@mcp.tool()
def replace_scene(document: dict[str, Any], expected_revision: int) -> dict[str, Any]:
    """Replace the complete versioned document in one undoable operation. Preserve unknown extension metadata."""
    return bridge.request("replace", {"document": document}, expected_revision)


@mcp.tool()
def undo(expected_revision: int) -> dict[str, Any]:
    """Undo the last document transaction from either the UI or MCP."""
    return bridge.request("undo", revision=expected_revision)


@mcp.tool()
def redo(expected_revision: int) -> dict[str, Any]:
    """Redo one document transaction."""
    return bridge.request("redo", revision=expected_revision)


@mcp.tool()
def set_playback(time: float | None = None, playing: bool | None = None,
                 rate: float | None = None, step: int | None = None) -> dict[str, Any]:
    """Seek, play/pause, change speed or reveal step. Does not add document history. Time is seconds."""
    return bridge.request("playback", {k: v for k, v in locals().items() if v is not None})


@mcp.tool()
def save_scene(path: str, expected_revision: int, overwrite: bool = False) -> dict[str, Any]:
    """Save UTF-8 scene JSON inside the workspace. Existing files need explicit overwrite=true."""
    return bridge.request("save", {"path": scene_path(root, path), "overwrite": overwrite}, expected_revision)


@mcp.tool()
def load_scene(path: str, expected_revision: int) -> dict[str, Any]:
    """Load scene JSON from the workspace; whole-scene validation and undo apply."""
    return bridge.request("load", {"path": scene_path(root, path)}, expected_revision)


if __name__ == "__main__":
    if options.start_editor:
        if not bridge.available():
            subprocess.Popen([str(options.start_editor.resolve()), "--pipe", options.pipe], cwd=root,
                             stdin=subprocess.DEVNULL, stdout=sys.stderr, stderr=sys.stderr,
                             creationflags=subprocess.CREATE_NO_WINDOW)
            bridge.request("get_scene")
        bridge.request("show_editor")
    mcp.run(transport="stdio")
