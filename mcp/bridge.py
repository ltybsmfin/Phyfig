"""Local named-pipe transport. It edits the live C++ document, never a shadow copy."""
from __future__ import annotations

import json
import re
import time
from pathlib import Path

import pywintypes
import win32file
import win32pipe


class EditorBridge:
    def __init__(self, pipe: str = "physics-editor"):
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,100}", pipe):
            raise ValueError("Invalid pipe name")
        self.name = "\\\\.\\pipe\\" + pipe

    def available(self) -> bool:
        try:
            win32pipe.WaitNamedPipe(self.name, 50)
            return True
        except pywintypes.error:
            return False

    def request(self, method: str, args: dict | None = None, revision: int | None = None, scope: str = "scene") -> dict:
        request = {"method": method, "args": args or {}, "scope": scope}
        if revision is not None:
            request["expectedRevision"] = revision
        data = (json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        if len(data) > 1_000_000:
            raise ValueError("Request exceeds 1 MB")
        deadline = time.monotonic() + 8
        while True:
            try:
                handle = win32file.CreateFile(self.name, win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                                              0, None, win32file.OPEN_EXISTING, 0, None)
                break
            except pywintypes.error as exc:
                if exc.winerror not in (2, 231) or time.monotonic() > deadline:
                    raise RuntimeError(f"Editor unavailable (Windows {exc.winerror}); start physics_editor with the matching --pipe name") from exc
                time.sleep(0.04)
        try:
            win32file.WriteFile(handle, data)
            response = bytearray()
            while time.monotonic() < deadline:
                _, available, _ = win32pipe.PeekNamedPipe(handle, 0)
                if available:
                    _, chunk = win32file.ReadFile(handle, min(available, 65536))
                    response.extend(chunk)
                    if len(response) > 1_000_000:
                        raise RuntimeError("Editor response exceeds 1 MB")
                    if b"\n" in response:
                        result = json.loads(response.split(b"\n", 1)[0])
                        if not result["ok"]:
                            raise ValueError(result["error"])
                        return result["result"]
                time.sleep(0.01)
            # An ambiguous mutation timeout is NOT retried: caller must read the revision.
            raise TimeoutError("Editor response timed out; query scene before retrying a mutation")
        finally:
            win32file.CloseHandle(handle)


def scene_path(root: Path, path: str) -> str:
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root.resolve()) or resolved.suffix.lower() != ".json":
        raise ValueError("Scene path must be a .json file inside the configured workspace")
    return str(resolved)
