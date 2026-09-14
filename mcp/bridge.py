"""Local named-pipe transport. It edits the live C++ document, never a shadow copy."""
from __future__ import annotations

import collections
import json
import os
import queue
import re
import subprocess
import threading
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


class StdioEditorBridge:
    """Explicit JSON-lines transport over the editor's stdin/stdout.

    The bridge starts its own C++ editor process with ``--stdio`` (never a named
    pipe, never an implicit fallback). Wire format is identical to
    ``EditorBridge``: one UTF-8 JSON request per line, one
    ``{"ok":true,"result":...}`` / ``{"ok":false,"error":"..."}`` response per
    line. A timed-out request is never retried and permanently marks the
    connection unusable, so a late response can never be matched to a later
    request. ``close()`` only ever touches the child this instance started.
    """

    def __init__(self, executable, args=(), cwd=None, stdio_arg="--stdio", timeout=8.0,
                 env=None, stderr_sink=None, stderr_limit=65536):
        self.executable = Path(executable)
        self.timeout = float(timeout)
        self._lock = threading.Lock()
        self._responses: queue.Queue = queue.Queue(maxsize=64)
        self._stderr_lock = threading.Lock()
        self._stderr_lines: collections.deque = collections.deque()
        self._stderr_bytes = 0
        self._stderr_limit = int(stderr_limit)
        self._stderr_sink = stderr_sink
        self._broken = False
        self._closed = False
        command = [str(self.executable)]
        if stdio_arg:
            command.append(str(stdio_arg))
        command.extend(str(arg) for arg in args)
        self.command = command
        try:
            self._process = subprocess.Popen(
                command, cwd=str(cwd) if cwd else None, env=env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except OSError as exc:
            # Nothing was started, so there is nothing to reap; report the exact
            # command instead of a bare Windows error.
            raise RuntimeError(f"Cannot start the stdio editor {command[0]!r}: {exc}") from exc
        try:
            self._reader = threading.Thread(target=self._read_stdout, name="stdio-editor-stdout", daemon=True)
            self._reader.start()
            self._drain = threading.Thread(target=self._read_stderr, name="stdio-editor-stderr", daemon=True)
            self._drain.start()
        except RuntimeError:
            # The child is ours and already running: never leak it just because
            # the pump threads could not start.
            self.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    @property
    def process(self):
        """The owned child process, for diagnostics and tests."""
        return self._process

    def available(self) -> bool:
        return not self._closed and not self._broken and self._process.poll() is None

    def stderr_text(self) -> str:
        """Bounded tail of the child's stderr (never blocks on a full pipe)."""
        with self._stderr_lock:
            return "\n".join(self._stderr_lines)

    def request(self, method: str, args: dict | None = None, revision: int | None = None,
                scope: str = "scene") -> dict:
        request = {"method": method, "args": args or {}, "scope": scope}
        if revision is not None:
            request["expectedRevision"] = revision
        data = (json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        if len(data) > 1_000_000:
            raise ValueError("Request exceeds 1 MB")
        with self._lock:
            self._check_usable()
            self._write(data)
            return self._read_response(self.timeout)

    def exchange_raw(self, payload: bytes, timeout: float | None = None) -> dict:
        """Diagnostics/tests only: send bytes verbatim and return the raw response object.

        Unlike ``request``, an ``{"ok":false,...}`` reply is returned as-is instead
        of raising, so protocol errors can be asserted on.
        """
        if not isinstance(payload, (bytes, bytearray)):
            raise TypeError("payload must be bytes")
        with self._lock:
            self._check_usable()
            self._write(bytes(payload))
            return self._read_response(self.timeout if timeout is None else float(timeout), raw=True)

    def send_raw(self, payload: bytes) -> None:
        """Diagnostics/tests only: write bytes verbatim without waiting for a reply.

        Used to queue several requests (or a request followed by EOF) before any
        response is consumed.
        """
        if not isinstance(payload, (bytes, bytearray)):
            raise TypeError("payload must be bytes")
        with self._lock:
            self._check_usable()
            self._write(bytes(payload))

    def next_response(self, timeout: float | None = None) -> dict:
        """Diagnostics/tests only: return the next raw response object already queued.

        Deliberately does not re-check the child's exit code: responses that were
        written before a normal EOF must still be readable after the child exits.
        """
        if self._closed:
            raise RuntimeError("Stdio editor bridge is closed")
        return self._read_response(self.timeout if timeout is None else float(timeout), raw=True)

    def close(self, timeout: float = 5.0) -> None:
        """Close stdin so the editor can end safely, then reap only this child.

        Every handle below belongs to ``self._process`` (the child this instance
        started): no named pipe is touched and no other editor or window is
        enumerated or signalled.
        """
        if self._closed:
            return
        self._closed = True
        try:
            if self._process.stdin is not None:
                self._process.stdin.close()
        except (OSError, ValueError):
            pass
        try:
            self._process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # Escalate only on our own, still-running child; an already reaped
            # process is never signalled (its pid may have been reused).
            if self._process.poll() is None:
                self._process.terminate()
            try:
                self._process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                if self._process.poll() is None:
                    self._process.kill()
                try:
                    self._process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    pass
        for stream in (self._process.stdout, self._process.stderr):
            try:
                if stream is not None:
                    stream.close()
            except (OSError, ValueError):
                pass

    def _check_usable(self) -> None:
        if self._closed:
            raise RuntimeError("Stdio editor bridge is closed")
        if self._broken:
            raise RuntimeError("Stdio editor connection is unusable after a timeout or protocol error; "
                               "close() it and start a new editor")
        code = self._process.poll()
        if code is not None:
            self._broken = True
            raise RuntimeError(f"Editor process exited with code {code}: {self.stderr_text()[-2000:]}")

    def _write(self, data: bytes) -> None:
        try:
            self._process.stdin.write(data)
            self._process.stdin.flush()
        except (OSError, ValueError) as exc:
            self._broken = True
            raise RuntimeError(f"Cannot write to the editor stdin: {exc}") from exc

    def _read_response(self, timeout: float, raw: bool = False) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self._broken = True
                raise TimeoutError("Editor response timed out; the connection is marked unusable and the request "
                                   "is NOT retried")
            try:
                line = self._responses.get(timeout=remaining)
            except queue.Empty:
                continue
            if line is None:
                self._broken = True
                raise RuntimeError(f"Editor closed its stdout before answering: {self.stderr_text()[-2000:]}")
            try:
                result = json.loads(line)
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                self._broken = True
                raise RuntimeError(f"Editor wrote a non-protocol line to stdout: {line[:200]!r}") from exc
            if not isinstance(result, dict):
                self._broken = True
                raise RuntimeError(f"Editor wrote a non-protocol line to stdout: {line[:200]!r}")
            if raw:
                return result
            if not result.get("ok"):
                raise ValueError(result.get("error", "Editor rejected the request"))
            return result["result"]

    def _read_stdout(self) -> None:
        stream = self._process.stdout
        try:
            while True:
                line = stream.readline()
                if not line:
                    break
                if len(line) > 1_000_000:
                    self._broken = True
                    break
                try:
                    self._responses.put_nowait(line)
                except queue.Full:
                    # Dropping a response would desynchronize request/response
                    # pairing; fail the connection instead of returning a stale one.
                    self._broken = True
                    break
        except (OSError, ValueError):
            pass
        finally:
            try:
                self._responses.put_nowait(None)
            except queue.Full:
                pass

    def _read_stderr(self) -> None:
        stream = self._process.stderr
        try:
            while True:
                line = stream.readline()
                if not line:
                    break
                text = line.decode("utf-8", "replace").rstrip("\r\n")
                with self._stderr_lock:
                    self._stderr_lines.append(text)
                    self._stderr_bytes += len(line)
                    while self._stderr_bytes > self._stderr_limit and self._stderr_lines:
                        self._stderr_bytes -= len(self._stderr_lines.popleft()) + 1
                if self._stderr_sink is not None:
                    try:
                        self._stderr_sink.write(text + "\n")
                        self._stderr_sink.flush()
                    except (OSError, ValueError):
                        self._stderr_sink = None
        except (OSError, ValueError):
            pass


def scene_path(root: Path, path: str) -> str:
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root.resolve()) or resolved.suffix.lower() != ".json":
        raise ValueError("Scene path must be a .json file inside the configured workspace")
    return str(resolved)
