"""Defect acceptance for the live --stdio editor: R1/R2/A2/A4/U4.

Run by Codex with the Debug or Release physics_editor path. Every run starts its
own --stdio children with a throwaway workspace under build/defect-acceptance and
never attaches to, resizes or closes a user's editor process or named pipe.

Covered here:
  R1  subscript text renders without missing-glyph warnings; a non-BMP codepoint
      is reported through get_view_state.textWarnings.
  R2  exporting the current composition at 2560 px wide with normal 18/24 px text
      must scale the raster above the 18 px base: the offscreen worker log must
      report requested > 18 and actual > 18, and the 2560 px PNG must exist and
      visibly contain more than one colour.
  A2  a component scope that was never presented (the project layout disables
      followMcp, so opening the draft neither switches nor shows the front end)
      rejects view_mode=current and exports with view_mode=fit + explicit size,
      proving fit needs no cached canvas.
  A4  load_project of a saved .physproj leaves the project clean
      (get_project.dirty false) after the next frames.

Not covered, and not claimed as tested: U2 (a live view change must not dirty the
project) and U3 (a real ColorEdit4 drag producing exactly one history entry).
Both need actual editor-side input smoke hooks; --smoke-test only calls
beginColorEdit/commitColorEdit directly.
"""
import json
import os
import re
import struct
import sys
import time
import uuid
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mcp"))

from bridge import StdioEditorBridge  # noqa: E402

OUT = ROOT / "build" / "defect-acceptance"


def editor_env():
    return {**os.environ, "VK_LAYER_VALIDATE_SYNC": "1"}


def png_stats(path):
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    width, height = struct.unpack(">II", data[16:24])
    offset, compressed = 8, b""
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        kind = data[offset + 4:offset + 8]
        if kind == b"IDAT":
            compressed += data[offset + 8:offset + 8 + length]
        offset += 12 + length
    raw = zlib.decompress(compressed)
    assert len(raw) == height * (width * 4 + 1), (path, width, height)
    colors = set()
    for y in range(0, height, max(1, height // 24)):
        row = raw[y * (width * 4 + 1) + 1:(y + 1) * (width * 4 + 1)]
        for x in range(0, width, max(1, width // 24)):
            colors.add(row[x * 4:x * 4 + 3])
    return width, height, len(colors)


def even_height(target_width, canvas_width, canvas_height):
    value = int(round(target_width * float(canvas_height) / float(canvas_width)))
    value = max(16, min(4096, value))
    if value % 2:
        value = value + 1 if value < 4096 else value - 1
    return value


def wait_job(bridge, timeout=240):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        state = bridge.request("get_job", {"kind": "export"})
        if state["status"] != "running":
            return state
        time.sleep(0.1)
    raise AssertionError("export job did not finish")


def wait_view(bridge, predicate, what, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        view = bridge.request("get_view_state")
        if predicate(view):
            return view
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def check_r1(bridge):
    scene = bridge.request("get_scene")
    commands = [{"op": "delete", "id": o["id"]}
                for o in scene["document"]["objects"] if o["type"] == "text"]
    commands += [
        {"op": "create", "id": "r1-sub", "type": "text", "name": "R1 subscript",
         "properties": {"x": 1, "y": 8, "text": "m\u2081 v\u2082", "size": 18}},
        {"op": "create", "id": "r1-emoji", "type": "text", "name": "R1 non-BMP",
         "properties": {"x": 1, "y": 6, "text": "m\u2081v\u2082\U0001F600", "size": 24}},
    ]
    bridge.request("apply", {"commands": commands}, scene["revision"])
    view = wait_view(bridge, lambda v: v["canvasReady"] and v["textWarningCount"] > 0, "text warnings")
    warnings = view["textWarnings"]
    assert not any(w["object"] == "r1-sub" for w in warnings), warnings
    assert any(w["object"] == "r1-emoji" and w["codepoint"] == 0x1F600 for w in warnings), warnings


def check_r2(bridge, stamp, data_root):
    revision = bridge.request("get_scene")["revision"]
    bridge.request("apply", {"commands": [
        {"op": "delete", "id": "r1-emoji"},
        {"op": "create", "id": "r2-normal", "type": "text", "name": "R2 normal text",
         "properties": {"x": 1, "y": 4, "text": "R2 raster probe", "size": 24}},
        {"op": "settings", "duration": 1},
    ]}, revision)
    view = wait_view(bridge, lambda v: v["canvasReady"] and v["canvasWidth"] and v["canvasHeight"],
                     "recorded canvas view")
    width = 2560
    height = even_height(width, view["canvasWidth"], view["canvasHeight"])
    revision = bridge.request("get_scene")["revision"]
    frames = OUT / f"r2-frames-{stamp}"
    job = bridge.request("export_animation", {"output": str(frames), "format": "png", "fps": 1,
        "width": width, "height": height, "view_mode": "current", "validation": True}, revision)
    done = wait_job(bridge)
    assert done["status"] == "completed" and done["id"] == job["id"], done
    assert done["viewport"]["mode"] == "current" and done["viewport"]["scope"] == "scene", done
    worker = (data_root / "build" / "jobs" / done["id"] / "worker.log").read_text(encoding="utf-8")
    matches = re.findall(r"requested exportFontPixelSize (\S+) -> actual raster (\S+) px", worker)
    assert matches, worker[-2000:]
    requested, actual = float(matches[-1][0]), float(matches[-1][1])
    assert requested > 18.0 and actual > 18.0, (requested, actual)
    images = sorted(frames.glob("frame_*.png"))
    assert images, frames
    actual_width, actual_height, colors = png_stats(images[0])
    assert (actual_width, actual_height) == (width, height) and colors > 1, (actual_width, actual_height, colors)
    assert "Validation errors: 0; warnings: 0" in worker, worker[-2000:]
    return requested, actual


def check_a2(bridge, stamp):
    # There is no set_follow_mcp method: follow is a project layout flag, so save
    # a project, flip layout.followMcp and load it back. Opening the draft then
    # neither switches the front end nor records a component canvas.
    project = bridge.request("get_project")
    package = OUT / f"a2-follow-off-{stamp}.physproj"
    bridge.request("save_project", {"path": str(package), "expectedToken": project["token"], "overwrite": True})
    document = json.loads(package.read_text(encoding="utf-8"))
    assert document["layout"]["followMcp"] is True, document["layout"]
    document["layout"]["followMcp"] = False
    package.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
    project = bridge.request("get_project")
    bridge.request("load_project", {"path": str(package), "expectedToken": project["token"], "discard": True})
    before = bridge.request("get_view_state").get("presentedScope")
    bridge.request("open_component", {"id": "independent_draft"})
    for _ in range(2):  # followMcp is off: the front end must stay on the scene
        assert bridge.request("get_view_state").get("presentedScope") == before, "front end followed the draft"
    draft = bridge.request("get_scene", scope="component")
    bridge.request("apply", {"commands": [{"op": "settings", "duration": 1}]}, draft["revision"], scope="component")
    draft = bridge.request("get_scene", scope="component")
    try:
        started = bridge.request("export_animation", {"output": str(OUT / f"a2-current-{stamp}"),
            "format": "png", "view_mode": "current", "width": 0, "height": 0}, draft["revision"], scope="component")
    except ValueError as error:
        assert "fit" in str(error), error
    else:
        bridge.request("cancel_job", {"kind": "export", "id": started["id"]})
        raise AssertionError("current export was accepted although the component was never presented")
    frames = OUT / f"a2-fit-{stamp}"
    job = bridge.request("export_animation", {"output": str(frames), "format": "png", "fps": 1,
        "width": 640, "height": 360, "view_mode": "fit", "validation": True}, draft["revision"], scope="component")
    done = wait_job(bridge)
    assert done["status"] == "completed" and done["id"] == job["id"], done
    assert done["viewport"]["mode"] == "fit" and done["viewport"]["scope"] == "component", done
    images = sorted(frames.glob("frame_*.png"))
    assert images and png_stats(images[0])[:2] == (640, 360), frames
    assert bridge.request("get_view_state").get("presentedScope") == before, "fit export switched the front end"
    bridge.request("close_component", {"discard": True})
    return done["viewport"]


def check_a4(executable, root, log):
    workspace = root / "a4"
    workspace.mkdir()
    with StdioEditorBridge(executable, cwd=workspace, env=editor_env(), stderr_sink=log) as bridge:
        scene = bridge.request("get_scene")
        bridge.request("apply", {"commands": [{"op": "create", "id": "a4-layout", "type": "point",
                                               "properties": {"x": 1, "y": 1}}]}, scene["revision"])
        project_path = workspace / "a4.physproj"
        bridge.request("save_project", {"path": str(project_path),
                                        "expectedToken": bridge.request("get_project")["token"], "overwrite": True})
        revision = bridge.request("get_scene")["revision"]
        bridge.request("apply", {"commands": [{"op": "move", "id": "a4-layout", "dx": 2, "dy": 0}]}, revision)
        assert bridge.request("get_project")["dirty"] is True, "content change did not dirty the project"
        bridge.request("load_project", {"path": str(project_path),
                                        "expectedToken": bridge.request("get_project")["token"], "discard": True})
        for _ in range(2):  # let two frames apply the restored project before checking
            bridge.request("get_view_state")
        project = bridge.request("get_project")
        assert project["dirty"] is False, project
    assert bridge.process.returncode == 0, bridge.process.returncode


def check_frame_limit(bridge, output):
    scene = bridge.request("get_scene")
    duration = scene["document"]["duration"]
    scene = bridge.request("apply", {"commands": [{"op": "settings", "duration": 121}]}, scene["revision"])
    try:
        bridge.request("export_animation", {"output": str(output), "fps": 30,
            "width": 320, "height": 180, "view_mode": "fit"}, scene["revision"])
    except ValueError as error:
        assert "3600" in str(error), error
    else:
        raise AssertionError("Export silently accepted more than 3600 frames")
    assert not output.exists(), output
    assert bridge.request("get_scene")["document"]["duration"] == 121
    bridge.request("apply", {"commands": [{"op": "settings", "duration": duration}]}, scene["revision"])


def run(executable):
    OUT.mkdir(parents=True, exist_ok=True)
    stamp = uuid.uuid4().hex
    root = OUT / f"run-{stamp}"
    root.mkdir()
    log_path = OUT / f"acceptance-{stamp}.log"
    with log_path.open("w", encoding="utf-8") as log:
        with StdioEditorBridge(executable, cwd=root, env=editor_env(), stderr_sink=log) as bridge:
            bridge.request("get_scene")
            check_r1(bridge)
            requested, actual = check_r2(bridge, stamp, root)
            check_a2(bridge, stamp)
            check_frame_limit(bridge, root / "over-limit.mp4")
            bridge.request("save_project", {"path": str(root / "acceptance.physproj"),
                                            "expectedToken": bridge.request("get_project")["token"], "overwrite": True})
        assert bridge.process.returncode == 0, bridge.process.returncode
        check_a4(executable, root, log)
    print(f"PASS: R1 subscript/non-BMP warnings; R2 2560 px export requested {requested:g} -> "
          f"actual {actual:g} px raster with a visible 2560 px PNG; A2 fit export of a never-presented "
          "component scope; A4 load_project stays clean; U4 >3600 frames rejected without retiming")
    print("Artifacts:", OUT / f"acceptance-{stamp}.log")


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve())
