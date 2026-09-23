"""用原生编辑器渲染冻结场景快照的帧，再封装成 PNG / MP4 / GIF。

流程::

    validate request -> 校验目标 -> 在目标同卷独占创建临时目录 physics-export-<32 位 hex>
      -> 在临时目录里独占写 ownership.json（所有权标记）
      -> 在 job 根目录原子发布 cleanup.json（清理凭据）
      -> 写 request 副本（加入 framesDirectory 字段）
      -> 原生 editor --export-job <request 副本> [--validation]
      -> 校验 result.json 以及 frame_%06d.png 的数量和尺寸
      -> PNG: 写 export.json 清单后把目录原子移动到目标
         MP4/GIF: ffmpeg 编码到临时文件后再提交到目标
      -> 无论成败都清理临时目录，确认删干净后丢掉 cleanup.json

约束：

* 进程隐藏窗口、``shell=False``、timeout 有界；进程树由父进程的 Windows
  JobObject 管理，取消父任务会终止全部子进程；环境变量原样继承
  （父进程设置的 ``VK_LAYER_VALIDATE_SYNC=1`` 会传给原生 editor）；
* 请求可选 ``validation: true``：为真时给原生命令追加 ``--validation``，
  缺省为 false，现有不带该字段的请求行为不变；
* ffmpeg 只加 ``-nostdin`` 与 ``-y``，且 ``-y`` 只作用于自己的临时文件；
  不加任何缩放滤镜，不会偷偷改变分辨率；
* 失败只写临时目录，不会破坏已有目标；overwrite 在启动与提交时各检查一次；
  ``overwrite=false`` 用 ``os.rename`` 提交（Windows 拒绝既存目标，避免
  "检查-提交"竞态覆盖），``overwrite=true`` 才用 ``os.replace``；
* 任何失败都写成结构化 error（调用端读 ``error.message``），并保证 worker
  自己退出，不影响编辑器存活。

临时目录的清理凭据（``<job root>/cleanup.json``）::

    {
      "version": 1,
      "marker": "physics-export-cleanup",
      "token": "<32 位小写十六进制>",   # 与临时目录名 physics-export-<token> 完全一致
      "jobRoot": "<绝对路径>",          # 本 worker 的运行目录
      "outputParent": "<绝对路径>",     # 临时目录的父目录，也就是目标所在目录
      "tempDirectory": "<绝对路径>",    # outputParent/physics-export-<token>
      "target": "<绝对路径>"            # 请求里的导出目标，绝不是临时目录
    }

* 顺序是“先独占建目录、再写内部所有权标记、最后发布凭据”：只有本任务
  ``os.mkdir`` 成功创建的目录才会带上所有权标记，而凭据只指向这样的目录并在
  三者齐备之后才发布。因此任何时刻 worker 被杀：
  - 发布凭据之前被杀，只可能留下一个**没有凭据**的目录/半成品，编辑器绝不会去
    动没有凭据的目录；
  - 撞名（目录已存在）时 ``os.mkdir`` 抛 ``FileExistsError``，直接换 token 重来，
    既不写标记也不写凭据，那个目录原样留着。
* 正常结束时先 ``rmtree`` 临时目录，确认目录真的不在了才删除凭据；删不干净就把
  凭据留给编辑器侧的 BackgroundJob，由它按同一套绑定校验清理（见
  ``apps/editor/background_job.hpp`` 的 cleanup 契约）。
* 编辑器只删“凭据与 start 之前缓存的 request 目标一致、名字正好是
  ``physics-export-`` + token、并且目录里带着匹配的 ownership.json”的目录，绝不按
  progress 里的路径删除，也绝不做目录扫描。

临时目录里的所有权标记（``<tempDirectory>/ownership.json``）::

    {
      "version": 1,
      "marker": "physics-export-ownership",
      "token": "<32 位小写十六进制>",   # 与目录名和凭据一致
      "jobRoot": "<绝对路径>",          # 本 worker 的运行目录
      "target": "<绝对路径>"            # 本次导出的目标
    }

* 标记写在**我们刚创建的那个目录内部**，用独占创建（``open(..., 'x')``）；编辑器
  删除前必须确认它是普通文件（不是符号链接/junction）且 token/jobRoot/target 与
  凭据一致，所以“凭据指向一个不是本任务创建的目录”这件事永远删不动东西。

实时进度（``<job root>/progress.json``，job root 就是 ``--input request.json``
所在目录，也是编辑器 BackgroundJob 的运行目录）::

    {
      "version": 1,
      "stage": "prepare" | "render" | "encode" | "commit" | "done" | "failed",
      "phase": "prepare" | "render" | "palettegen" | "encode" | "commit" | "done",
      "completedFrames": int, "totalFrames": int,
      "elapsedSeconds": float,      # 本 worker 从启动到现在的墙钟
      "phaseFraction": float|null,  # 当前阶段（phase）内部的 0..1
      "overallFraction": float,     # 整体 0..1，单调不减
      "fps": float|null,            # 渲染实测帧率；没有稳定样本时为 null
      "estimatedRemainingSeconds": float|null,   # 当前阶段的剩余估计
      "estimateScope": "render"|"palettegen"|"encode"|null,
      "renderSeconds": float|null, "readbackWriteSeconds": float|null,
      "message": str                # 仅失败时出现
    }

* 每次都是完整 JSON 写入临时文件后 ``os.replace`` 成目标名，读者只会看到完整
  的旧版本或新版本；发布始终是尽力而为，进度写不出去绝不导致导出失败。
* ``completedFrames`` 只来自渲染 worker 发布的 ``framesDirectory/progress.json``，
  该文件缺失或损坏时退化为统计目录里真实存在的 ``frame_%06d.png``；两种来源都
  取历史最大值，绝不凭时间猜帧数。
* 渲染 100% 不等于导出 100%：PNG 的渲染最多映射到 0.98，MP4/GIF 的渲染最多
  映射到 0.90，只有提交（原子改名/替换）成功之后才发布 ``stage="done"`` 与
  ``overallFraction=1.0``。取消或失败保留最后进度，不会跳成 100%。
* 编码阶段用 ``ffmpeg -progress <file>`` 的真实进度；GIF 的 palettegen 与
  encode 两个 pass 分别是 ``phase="palettegen"`` / ``phase="encode"``。

CLI::

    python export_animation.py --input request.json --output result.json

成功 ``result.json``：``{ok, output, format, frames, width, height, fps, duration}``
失败 ``result.json``：``{ok: false, error: {code, message, detail?}}``
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid

SUPPORTED_FORMATS = ('png', 'mp4', 'gif')
FRAME_PATTERN = 'frame_%06d.png'
FRAME_RE = re.compile(r'^frame_(\d{6})\.png$')

# 临时目录名与父进程清理凭据：临时目录叫 physics-export-<token>，同一个 token 也
# 写进目录内的 ownership.json 与 job 根目录的 cleanup.json。编辑器据此只删“名字、
# 父目录、目标三者都对得上、并且目录里带着匹配所有权标记”的那一个目录。
TEMP_PREFIX = 'physics-export-'
CLEANUP_NAME = 'cleanup.json'
CLEANUP_TMP_NAME = 'cleanup.json.tmp'
CLEANUP_VERSION = 1
CLEANUP_MARKER = 'physics-export-cleanup'
CLEANUP_TOKEN_RE = re.compile(r'^[0-9a-f]{32}$')
OWNERSHIP_NAME = 'ownership.json'
OWNERSHIP_MARKER = 'physics-export-ownership'

MIN_SIZE, MAX_SIZE = 16, 4096
MIN_FPS, MAX_FPS = 1, 60
MIN_FRAMES, MAX_FRAMES = 1, 3600
MAX_REQUEST_BYTES = 1 << 20

LOG_TAIL = 4000
NATIVE_TIMEOUT_BASE = 180.0
# 每帧预算与上限都放宽到支持小时级长片：2K 下一代长片每帧约 2 s 渲染，
# 2500 帧就要一个多小时，原来的 3600 s 上限会把正常导出判死。上限仍保留，
# 用来兜住真正卡死的渲染进程（编辑器侧已经不再对导出设超时）。
NATIVE_TIMEOUT_PER_FRAME = 6.0
NATIVE_TIMEOUT_MAX = 21600.0
FFMPEG_TIMEOUT_BASE = 60.0
FFMPEG_TIMEOUT_PER_FRAME = 0.5
FFMPEG_TIMEOUT_MAX = 3600.0

# --- 实时进度参数 -----------------------------------------------------------
PROGRESS_VERSION = 1
PROGRESS_NAME = 'progress.json'
PROGRESS_TMP_NAME = 'progress.json.tmp'
PROGRESS_MAX_BYTES = 64 * 1024
PROGRESS_WRITE_INTERVAL = 0.15   # 值有变化时的最快发布节奏
PROGRESS_HEARTBEAT = 1.0         # 值没变化时也会刷新的节奏（保证 elapsed 前进）
PROGRESS_TICK = 0.1              # 等待子进程时的监测节奏
FRAME_SCAN_INTERVAL = 0.25       # 退化到统计帧文件时的目录扫描间隔
ESTIMATE_WINDOW = 1.0            # 算稳定速率所需的最短时间窗口（秒）
ESTIMATE_MEMORY = 8.0            # 速率样本保留窗口（秒）
# 阶段在整体比例里的分布：渲染结束不等于导出结束，提交成功才是 100%。
PREPARE_END = 0.02
COMMIT_START = 0.995
RENDER_END = {'png': 0.98, 'mp4': 0.90, 'gif': 0.90}
ENCODE_PHASES = ('palettegen', 'encode')


def _make_temp_dir(prefix, parent=None):
    """创建临时目录并返回路径。

    ``tempfile.mkdtemp`` 固定用 mode 0o700；本机的文件沙箱会拒绝在 0o700 目录里
    再创建文件（WinError 5）。Windows 上 POSIX mode 对 ACL 基本无影响，所以
    Windows 用 0o755，POSIX 保持 0o700。
    """
    base = parent if parent is not None else tempfile.gettempdir()
    mode = 0o755 if os.name == 'nt' else 0o700
    for _ in range(64):
        candidate = os.path.join(base, prefix + uuid.uuid4().hex)
        try:
            os.mkdir(candidate, mode)
        except FileExistsError:
            continue
        return candidate
    raise OSError(f'无法创建临时目录：{base}')


def _cleanup_record(temp_root, parent, target, token, job_root):
    return {
        'version': CLEANUP_VERSION,
        'marker': CLEANUP_MARKER,
        'token': token,
        'jobRoot': job_root,
        'outputParent': parent,
        'tempDirectory': temp_root,
        'target': target,
    }


def _write_cleanup_record(path, record):
    """原子发布清理凭据：读者只会看到完整的旧版本或新版本。"""
    temporary = path.with_name(CLEANUP_TMP_NAME)
    with open(temporary, 'w', encoding='utf-8') as handle:
        json.dump(record, handle, ensure_ascii=False, indent=2)
    os.replace(temporary, path)


def _drop_cleanup_record(path):
    """丢掉凭据（连同可能残留的半成品文件）；失败不影响导出结果。"""
    for candidate in (path, path.with_name(CLEANUP_TMP_NAME)):
        try:
            os.remove(candidate)
        except OSError:
            pass


def _ownership_marker(token, job_root, target):
    return {
        'version': CLEANUP_VERSION,
        'marker': OWNERSHIP_MARKER,
        'token': token,
        'jobRoot': job_root,
        'target': target,
    }


def _write_ownership_marker(directory, token, job_root, target):
    """在刚创建的自有目录里独占写所有权标记。

    独占创建（``'x'``）是关键：标记只可能出现在本任务 ``os.mkdir`` 成功的那一个
    目录里，编辑器删除前核对它，就不会把别的目录当成自己的临时目录。
    """
    path = os.path.join(directory, OWNERSHIP_NAME)
    with open(path, 'x', encoding='utf-8') as handle:
        json.dump(_ownership_marker(token, job_root, target), handle, ensure_ascii=False, indent=2)


def _make_recorded_temp_dir(parent, record_path, target):
    """独占建目录 -> 独占写目录内所有权标记 -> 发布凭据，返回临时目录路径。

    顺序是刻意的：``os.mkdir`` 成功才说明这个目录是本任务创建的，标记随即写进目录
    内部，凭据最后才原子发布。于是任何时刻被强杀都安全：
    * 发布凭据之前被杀：只留下一个没有凭据的目录/半成品，父进程不会去动；
    * ``os.mkdir`` 撞名：那不是本任务创建的目录，换 token 重来，既不写标记也不写
      凭据，撞名目录原样留着。
    """
    mode = 0o755 if os.name == 'nt' else 0o700
    job_root = os.path.dirname(os.path.abspath(str(record_path)))
    for _ in range(64):
        token = uuid.uuid4().hex
        temp_root = os.path.join(parent, TEMP_PREFIX + token)
        try:
            os.mkdir(temp_root, mode)
        except FileExistsError:
            continue
        try:
            _write_ownership_marker(temp_root, token, job_root, target)
            _write_cleanup_record(
                record_path, _cleanup_record(temp_root, parent, target, token, job_root))
        except OSError:
            # 凭据还没有发布（或发布失败）：这个目录尚未被任何凭据登记，尽量自己
            # 删掉；删不掉也只是留下一个父进程不会动它的残留目录。
            _drop_cleanup_record(record_path)
            shutil.rmtree(temp_root, ignore_errors=True)
            raise
        return temp_root
    raise OSError(f'无法创建临时目录：{parent}')


class ExportError(Exception):
    """带结构化 code 的导出失败。"""

    def __init__(self, code, message, detail=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.detail = detail

    def to_json(self):
        payload = {'code': self.code, 'message': self.message}
        if self.detail:
            payload['detail'] = self.detail
        return payload


def _timeout(base, per_frame, cap, frames):
    return min(cap, base + per_frame * frames)


def _require_int(request, key, low, high):
    value = request.get(key)
    if isinstance(value, bool):
        raise ExportError('invalid_request', f'{key} 必须是整数')
    if isinstance(value, float) and value.is_integer():
        value = int(value)
    if not isinstance(value, int):
        raise ExportError('invalid_request', f'{key} 必须是整数')
    if not low <= value <= high:
        raise ExportError('invalid_request', f'{key} 必须在 {low}..{high} 之间')
    return value


def _require_number(request, key):
    value = request.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ExportError('invalid_request', f'{key} 必须是数字')
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise ExportError('invalid_request', f'{key} 必须是有限正数（秒）')
    return value


def _require_bool(request, key, default=False):
    value = request.get(key, default)
    if not isinstance(value, bool):
        raise ExportError('invalid_request', f'{key} 必须是布尔值')
    return value


def _require_abs_path(request, key):
    value = request.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ExportError('invalid_request', f'{key} 必须是非空字符串路径')
    path = Path(value)
    if not path.is_absolute():
        raise ExportError('invalid_request', f'{key} 必须是绝对路径：{value}')
    return os.path.normpath(str(path))


def resolve_ffmpeg(value, find):
    if not isinstance(value, str) or not value.strip():
        raise ExportError('invalid_request', "ffmpeg 必须是 'ffmpeg' 或可执行文件路径")
    candidate = value.strip()
    if os.path.dirname(candidate):
        path = os.path.abspath(candidate)
        if not os.path.isfile(path):
            raise ExportError('ffmpeg_missing', f'找不到 ffmpeg：{path}')
        return path
    found = find(candidate)
    if not found:
        raise ExportError('ffmpeg_missing', f'PATH 中找不到 {candidate}；请在请求里给出 ffmpeg 绝对路径')
    return os.path.abspath(found)


def validate_request(request, *, find=None):
    """校验并规范化请求；返回配置字典，不触碰文件系统。"""
    if not isinstance(request, dict):
        raise ExportError('invalid_request', '请求必须是 JSON 对象')
    find = find or shutil.which

    executable = _require_abs_path(request, 'executable')
    snapshot = _require_abs_path(request, 'snapshot')
    output = _require_abs_path(request, 'output')
    if not os.path.isfile(executable):
        raise ExportError('not_found', f'找不到原生编辑器可执行文件：{executable}')
    if not os.path.isfile(snapshot):
        raise ExportError('not_found', f'找不到场景快照：{snapshot}')

    raw_format = request.get('format')
    if not isinstance(raw_format, str) or raw_format.lower() not in SUPPORTED_FORMATS:
        raise ExportError('unsupported_format', "format 必须是 'png'、'mp4' 或 'gif'")
    fmt = raw_format.lower()

    width = _require_int(request, 'width', MIN_SIZE, MAX_SIZE)
    height = _require_int(request, 'height', MIN_SIZE, MAX_SIZE)
    if width % 2 or height % 2:
        raise ExportError('invalid_request', 'width/height 必须是偶数（yuv420p 与安全裁剪要求）')

    fps = _require_int(request, 'fps', MIN_FPS, MAX_FPS)
    duration = _require_number(request, 'duration')
    expected = int(math.ceil(duration * fps - 1e-9))
    if expected > MAX_FRAMES:
        raise ExportError(
            'invalid_request',
            f'duration×fps 需要 {expected} 帧，超过 {MAX_FRAMES} 帧上限；请降低 fps 或缩短 duration')
    frames = _require_int(request, 'frames', MIN_FRAMES, MAX_FRAMES)
    if frames != expected:
        raise ExportError(
            'invalid_request',
            f'frames 必须等于 ceil(duration×fps)={expected}（收到 {frames}）；不会自动丢帧或变速')
    if abs(frames / fps - duration) > 1.0 / fps:
        raise ExportError('invalid_request', '编码时长与请求 duration 的误差必须小于 1/fps')

    overwrite = _require_bool(request, 'overwrite')
    validation = _require_bool(request, 'validation')
    ffmpeg = None
    if fmt != 'png':
        ffmpeg = resolve_ffmpeg(request.get('ffmpeg'), find)

    return {
        'request': request,
        'executable': executable,
        'snapshot': snapshot,
        'output': output,
        'format': fmt,
        'width': width,
        'height': height,
        'fps': fps,
        'frames': frames,
        'duration': duration,
        'overwrite': overwrite,
        'validation': validation,
        'ffmpeg': ffmpeg,
    }


def validate_target(config):
    """检查目标与父目录；返回父目录绝对路径。"""
    output = config['output']
    parent = os.path.dirname(output)
    if not parent:
        raise ExportError('invalid_request', f'output 必须位于已有目录中：{output}')
    if not os.path.isdir(parent):
        raise ExportError('not_found', f'output 的父目录不存在：{parent}')

    if config['format'] == 'png':
        if os.path.exists(output):
            raise ExportError(
                'target_exists',
                f'PNG 目标已存在（文件或目录）：{output}；为安全起见不支持覆盖已有目录，请换一个新路径')
        return parent

    suffix = '.' + config['format']
    if not output.lower().endswith(suffix):
        raise ExportError('invalid_request', f'{config["format"].upper()} 目标扩展名必须是 {suffix}：{output}')
    if os.path.exists(output):
        if os.path.isdir(output):
            raise ExportError('invalid_request', f'目标路径是目录：{output}')
        if not config['overwrite']:
            raise ExportError('target_exists', f'目标已存在且 overwrite=false：{output}')
    return parent


def _tail(data, limit=LOG_TAIL):
    if not data:
        return ''
    text = data.decode('utf-8', errors='replace') if isinstance(data, bytes) else str(data)
    text = text.strip()
    if len(text) > limit:
        text = '...' + text[-limit:]
    return text


def _run_hidden(argv, timeout, *, timeout_code='process_timeout', cwd=None):
    """隐藏窗口、无 shell 运行 argv；超时杀掉直接子进程并抛 ExportError。

    整棵进程树由父进程的 Windows JobObject 管理，这里只负责直接子进程
    （native editor / ffmpeg 都是直接子进程，不会再委托别的命令行工具）。
    """
    creationflags = 0
    startupinfo = None
    if os.name == 'nt':
        creationflags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    try:
        process = subprocess.Popen(
            list(argv), cwd=cwd, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            shell=False, creationflags=creationflags, startupinfo=startupinfo,
        )
    except OSError as error:
        raise ExportError('launch_failed', f'无法启动进程：{argv[0]}', str(error)) from error
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        process.kill()
        try:
            output, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            output = b''
        raise ExportError(timeout_code, f'进程超过 {timeout:.0f}s 未结束：{os.path.basename(argv[0])}',
                          _tail(output)) from error
    return process.returncode, _tail(output)


def _png_size(path):
    with open(path, 'rb') as handle:
        header = handle.read(24)
    if (len(header) < 24 or header[:8] != b'\x89PNG\r\n\x1a\n'
            or header[12:16] != b'IHDR'):
        raise ExportError('frame_invalid', f'不是有效的 PNG 文件：{path}')
    return int.from_bytes(header[16:20], 'big'), int.from_bytes(header[20:24], 'big')


def verify_frames(frames_dir, frames, width, height):
    """要求恰好 frame_000000..frame_{frames-1}，且尺寸与请求一致。"""
    found = {}
    for name in os.listdir(frames_dir):
        match = FRAME_RE.match(name)
        if match:
            found[int(match.group(1))] = os.path.join(frames_dir, name)
    expected = set(range(frames))
    missing = sorted(expected - set(found))
    if missing:
        raise ExportError('frame_missing', f'缺少 {len(missing)} 帧，例如 frame_{missing[0]:06d}.png')
    extra = sorted(set(found) - expected)
    if extra:
        raise ExportError('frame_unexpected', f'出现多余的帧 frame_{extra[0]:06d}.png')
    for index in range(frames):
        actual = _png_size(found[index])
        if actual != (width, height):
            raise ExportError(
                'frame_size_mismatch',
                f'frame_{index:06d}.png 尺寸 {actual[0]}x{actual[1]} 与请求 {width}x{height} 不符（不会偷偷缩放）')


def _clamp01(value):
    if value is None:
        return None
    try:
        value = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(value):
        return None
    return min(1.0, max(0.0, value))


def _finite(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    value = float(value)
    return value if math.isfinite(value) else None


def _pump(on_tick, progress):
    """执行一次监测回调并落盘。监测只读文件，任何异常都不许中断导出。"""
    if on_tick is not None:
        try:
            on_tick()
        except Exception:  # noqa: BLE001 - 监测永远不能让导出失败
            pass
    if progress is not None:
        try:
            progress.flush()
        except Exception:  # noqa: BLE001
            pass


def _run_monitored(run, argv, timeout, *, timeout_code, cwd, on_tick, progress):
    """在监测线程里跑阻塞 runner，主线程按固定节奏读取进度。

    runner 协议不变（``run(argv, timeout, *, timeout_code, cwd)``); 这里只是把
    它放到后台线程，好让本进程在子进程还活着的时候就能发布进度，而不用等它退出。
    """
    outcome = {}

    def target():
        try:
            outcome['value'] = run(argv, timeout, timeout_code=timeout_code, cwd=cwd)
        except BaseException as error:  # noqa: BLE001 - 原样交给主线程抛出
            outcome['error'] = error

    runner = threading.Thread(target=target, name='export-child', daemon=True)
    runner.start()
    try:
        while runner.is_alive():
            _pump(on_tick, progress)
            runner.join(PROGRESS_TICK)
    finally:
        # 子进程退出后补一次，最后一帧进度也必须落盘。
        _pump(on_tick, progress)
    if 'error' in outcome:
        raise outcome['error']
    return outcome['value']


class ProgressPublisher:
    """把导出阶段/计数/耗时映射成 job 根目录的原子 progress.json。

    单写者、单读者：每次写完整 JSON 再 ``os.replace``，读者只会看到完整的旧版本
    或新版本。发布失败（例如读者短暂占住目标文件）只算这一次没更新，下一次 tick
    会重试，绝不因此让导出失败。``clock`` 可注入，测试用假时钟做确定性验证。
    """

    def __init__(self, path, total_frames, fmt, *, clock=time.monotonic):
        self.path = Path(path)
        self.fmt = fmt
        self.total = max(1, int(total_frames))
        self.render_end = RENDER_END.get(fmt, 0.90)
        self.clock = clock
        self._start = clock()
        self._stage = 'prepare'
        self._phase = 'prepare'
        self._phase_fraction = None
        self._completed = 0
        self._overall = 0.0
        self._fps = None
        self._remaining = None
        self._estimate_scope = None
        self._render_seconds = None
        self._readback_seconds = None
        self._message = None
        self._frame_samples = []
        self._pass_samples = []
        self._passes = 1
        self._pass_floor = 0.0
        self._pass_share = 1.0
        self._signature = None
        self._written_at = float('-inf')
        self.writes = 0
        self.failures = 0

    # -- 渲染 ---------------------------------------------------------------
    def note_render(self, completed, *, render_seconds=None, readback_seconds=None):
        """渲染 worker（或帧文件统计）报告的真实完成帧数。"""
        completed = max(self._completed, int(completed))
        completed = min(completed, self.total)
        self._completed = completed
        render_seconds = _finite(render_seconds)
        if render_seconds is not None:
            self._render_seconds = render_seconds
        readback_seconds = _finite(readback_seconds)
        if readback_seconds is not None:
            self._readback_seconds = readback_seconds
        if self._stage in ('prepare', 'render'):
            self._stage = 'render'
        self._phase = 'render'
        self._estimate_scope = 'render'
        self._phase_fraction = completed / self.total
        self._overall = max(self._overall, PREPARE_END + (self.render_end - PREPARE_END) * self._phase_fraction)
        now = self.clock()
        if not self._frame_samples or completed != self._frame_samples[-1][1]:
            self._frame_samples.append((now, completed))
            del self._frame_samples[:-256]
        while len(self._frame_samples) > 2 and now - self._frame_samples[0][0] > ESTIMATE_MEMORY:
            self._frame_samples.pop(0)
        fps = self._rate(self._frame_samples, now)
        if fps is None and render_seconds and render_seconds >= ESTIMATE_WINDOW and completed >= 1:
            # 渲染 worker 自己的时钟也可信（它按帧计时）。
            fps = completed / render_seconds
        self._fps = fps
        if fps is None:
            self._remaining = None
        else:
            self._remaining = 0.0 if completed >= self.total else (self.total - completed) / fps
        self.flush()

    def render_complete(self):
        """渲染进程成功退出：帧数到满，但整体比例仍停在渲染段的末尾。"""
        self._completed = self.total
        if self._stage in ('prepare', 'render'):
            self._stage = 'render'
        self._phase = 'render'
        self._phase_fraction = 1.0
        self._overall = max(self._overall, self.render_end)
        self.flush(force=True)

    # -- 编码 ---------------------------------------------------------------
    def begin_encode(self, phase, *, passes=1):
        """进入一个 ffmpeg pass：palettegen 与 encode 是分开的、看得见的阶段。"""
        self._passes = max(1, int(passes))
        self._stage = 'encode'
        self._phase = phase
        self._phase_fraction = None
        self._pass_floor = 0.0 if self._passes == 1 or phase == 'palettegen' else 1.0 / self._passes
        self._pass_share = 1.0 if self._passes == 1 else 1.0 / self._passes
        self._pass_samples = []
        self._remaining = None
        self._estimate_scope = phase
        self._overall = max(self._overall, self.render_end)
        self.flush(force=True)

    def note_encode(self, phase, fraction):
        """fraction 是当前 pass 的真实进度；None 表示这一 pass 报不出比例。"""
        if phase != self._phase:
            self.begin_encode(phase, passes=self._passes)
        now = self.clock()
        fraction = _clamp01(fraction)
        if fraction is None:
            self._phase_fraction = None
            self._remaining = None
            self._overall = max(self._overall,
                                self.render_end + (COMMIT_START - self.render_end) * self._pass_floor)
            self.flush()
            return
        self._phase_fraction = fraction
        inner = self._pass_floor + self._pass_share * fraction
        self._overall = max(self._overall, self.render_end + (COMMIT_START - self.render_end) * inner)
        if not self._pass_samples or fraction != self._pass_samples[-1][1]:
            self._pass_samples.append((now, fraction))
            del self._pass_samples[:-256]
        while len(self._pass_samples) > 2 and now - self._pass_samples[0][0] > ESTIMATE_MEMORY:
            self._pass_samples.pop(0)
        rate = self._rate(self._pass_samples, now)
        self._remaining = None if rate is None else (1.0 - fraction) / rate
        self.flush()

    # -- 阶段与终点 ---------------------------------------------------------
    def set_stage(self, stage, *, overall=None, message=None, force=False):
        self._stage = stage
        self._phase = stage
        self._phase_fraction = None
        self._estimate_scope = None
        self._remaining = None
        if message is not None:
            self._message = message
        if overall is not None:
            self._overall = max(self._overall, float(overall))
        self.flush(force=force)

    def done(self):
        """只有目标已经原子提交成功之后才允许走到这里。"""
        self._stage = 'done'
        self._phase = 'done'
        self._completed = self.total
        self._phase_fraction = 1.0
        self._overall = 1.0
        self._remaining = 0.0
        self._estimate_scope = None
        self.flush(force=True)

    def fail(self, error):
        self._stage = 'failed'
        self._phase = 'failed'
        self._phase_fraction = None
        self._remaining = None
        self._estimate_scope = None
        # 任务已经结束，上一次实时吞吐是过期样本：终态载荷不再带 fps，这样它和
        # 编辑器在“最后一次发布没被读到”时由退出码/result.json 合成的终态同形，
        # 读者看到的字段集合不会随一次发布是否成功而变。
        self._fps = None
        self._message = getattr(error, 'message', None) or str(error)
        # 整体比例保留最后可信值：失败不会跳成 100%。
        self.flush(force=True)

    # -- 发布 ---------------------------------------------------------------
    @staticmethod
    def _rate(samples, now):
        if len(samples) < 2:
            return None
        first = samples[0]
        window = now - first[0]
        delta = samples[-1][1] - first[1]
        if window < ESTIMATE_WINDOW or delta <= 0:
            return None
        return delta / window

    def payload(self):
        elapsed = max(0.0, self.clock() - self._start)
        payload = {
            'version': PROGRESS_VERSION,
            'stage': self._stage,
            'phase': self._phase,
            'completedFrames': int(self._completed),
            'totalFrames': int(self.total),
            'elapsedSeconds': round(elapsed, 3),
            'phaseFraction': None if self._phase_fraction is None else round(_clamp01(self._phase_fraction), 6),
            'overallFraction': round(_clamp01(self._overall), 6),
            'fps': None if self._fps is None else round(self._fps, 3),
            'estimatedRemainingSeconds': None if self._remaining is None else round(max(0.0, self._remaining), 3),
            'estimateScope': self._estimate_scope,
        }
        if self._render_seconds is not None:
            payload['renderSeconds'] = round(self._render_seconds, 3)
        if self._readback_seconds is not None:
            payload['readbackWriteSeconds'] = round(self._readback_seconds, 3)
        if self._message:
            payload['message'] = self._message
        return payload

    def flush(self, *, force=False):
        """节流发布：值有变化才写（最快 PROGRESS_WRITE_INTERVAL），偶尔心跳。"""
        payload = self.payload()
        signature = (payload['stage'], payload['phase'], payload['completedFrames'],
                     payload['overallFraction'], payload['fps'],
                     payload['estimatedRemainingSeconds'])
        now = self.clock()
        if not force:
            if now - self._written_at < PROGRESS_WRITE_INTERVAL:
                return False
            if signature == self._signature and now - self._written_at < PROGRESS_HEARTBEAT:
                return False
        temporary = self.path.with_name(PROGRESS_TMP_NAME)
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            with open(temporary, 'w', encoding='utf-8') as handle:
                json.dump(payload, handle, ensure_ascii=False, indent=2)
            os.replace(temporary, self.path)
        except OSError:
            self.failures += 1
            return False
        self._signature = signature
        self._written_at = now
        self.writes += 1
        return True


class RenderProgressReader:
    """读取渲染 worker 发布的 ``framesDirectory/progress.json``。

    契约版本 1：``{version, stage, completedFrames, totalFrames, elapsedSeconds,
    renderSeconds, readbackWriteSeconds}``。文件缺失、空、被截断、损坏或版本/
    帧数对不上都只是“这次没有样本”，绝不抛出。渲染 worker 尚未发布进度时退化
    为统计目录里真实存在的 ``frame_%06d.png``，两种来源都只取历史最大值。
    """

    def __init__(self, frames_dir, total_frames, *, clock=time.monotonic,
                 scan_interval=FRAME_SCAN_INTERVAL):
        self.frames_dir = Path(frames_dir)
        self.total = max(0, int(total_frames))
        self.clock = clock
        self.scan_interval = scan_interval
        self._next_scan = float('-inf')
        self._counted = 0
        self.file_samples = 0
        self.scan_samples = 0

    def sample(self):
        payload = self._read_file()
        source = 'worker'
        if payload is None:
            payload = self._count_frames()
            source = 'frames'
        if payload is None:
            return None
        completed = payload.get('completedFrames')
        if isinstance(completed, bool) or not isinstance(completed, int) or completed < 0:
            return None
        payload['completedFrames'] = min(completed, self.total)
        payload['source'] = source
        return payload

    def _read_file(self):
        try:
            with open(self.frames_dir / PROGRESS_NAME, 'rb') as handle:
                data = handle.read(PROGRESS_MAX_BYTES + 1)
        except OSError:
            return None
        if not data or len(data) > PROGRESS_MAX_BYTES:
            return None
        try:
            payload = json.loads(data.decode('utf-8'))
        except (UnicodeDecodeError, ValueError):
            return None
        if not isinstance(payload, dict) or payload.get('version') != PROGRESS_VERSION:
            return None
        stage = payload.get('stage')
        if not isinstance(stage, str) or not stage:
            return None
        total = payload.get('totalFrames')
        if total is not None and (isinstance(total, bool) or not isinstance(total, int)
                                  or (self.total and total != self.total)):
            return None
        self.file_samples += 1
        return payload

    def _count_frames(self):
        now = self.clock()
        if now < self._next_scan:
            return None
        self._next_scan = now + self.scan_interval
        try:
            names = os.listdir(self.frames_dir)
        except OSError:
            return None
        count = 0
        for name in names:
            if FRAME_RE.match(name):
                count += 1
        count = min(count, self.total) if self.total else count
        self._counted = max(self._counted, count)
        self.scan_samples += 1
        if self._counted <= 0:
            return None
        return {'completedFrames': self._counted}


class EncodeProgressReader:
    """读取 ``ffmpeg -progress <file>`` 追加的 key=value 进度流。

    ffmpeg 每约 0.5 s 追加一个完整块并以 ``progress=continue|end`` 收尾；这里只
    读文件末尾的有限字节，取最后一个完整块，所以进度文件再大也不会拖慢导出。
    ``out_time_us`` 是首选比例来源（palettegen 只输出一帧，``frame=`` 不可用），
    它缺失时退化为 ``frame/frames``。
    """

    TAIL_BYTES = 64 * 1024

    def __init__(self, path, *, duration, frames, tail_bytes=TAIL_BYTES):
        self.path = Path(path)
        self.duration = float(duration) if duration else 0.0
        self.frames = int(frames) if frames else 0
        self.tail_bytes = tail_bytes
        self.blocks = 0

    def sample(self):
        try:
            with open(self.path, 'rb') as handle:
                handle.seek(0, os.SEEK_END)
                size = handle.tell()
                handle.seek(max(0, size - self.tail_bytes))
                data = handle.read()
        except OSError:
            return None
        if not data:
            return None
        block = {}
        complete = None
        for line in data.decode('utf-8', errors='replace').splitlines():
            key, separator, value = line.partition('=')
            if not separator:
                continue
            key = key.strip()
            value = value.strip()
            if key == 'progress':
                if block:
                    complete = dict(block)
                    self.blocks += 1
                    block = {}
                continue
            block[key] = value
        # 只采纳完整块；一个块都没写完时用当前的（比没有强，比例只会偏小）。
        if complete is None and block:
            complete = block
        if not complete:
            return None
        return self._fraction(complete)

    def _fraction(self, block):
        raw = block.get('out_time_us', block.get('out_time_ms'))
        seconds = None
        if raw is not None:
            try:
                seconds = int(raw) / 1e6
            except (TypeError, ValueError):
                seconds = None
        if seconds is not None and self.duration > 0:
            return _clamp01(seconds / self.duration)
        try:
            frame = int(block.get('frame', ''))
        except (TypeError, ValueError):
            return None
        if self.frames <= 0:
            return None
        return _clamp01(frame / self.frames)


def run_native_export(config, frames_dir, run, progress=None):
    """写 request 副本（含 framesDirectory）并调用原生 editor --export-job。

    progress 非空时，子进程还在跑的时候主线程就按 PROGRESS_TICK 的节奏读取
    ``framesDirectory/progress.json``（渲染 worker 每帧发布），映射成 job 根的
    进度，不需要等 native 退出。
    """
    timeout = _timeout(NATIVE_TIMEOUT_BASE, NATIVE_TIMEOUT_PER_FRAME,
                       NATIVE_TIMEOUT_MAX, config['frames'])
    job = dict(config['request'])
    for key in ('executable', 'snapshot', 'output', 'format', 'width', 'height',
                'fps', 'frames', 'duration', 'overwrite', 'validation'):
        job[key] = config[key]
    job['framesDirectory'] = frames_dir
    work = os.path.dirname(frames_dir)
    request_path = os.path.join(work, 'request.json')
    with open(request_path, 'w', encoding='utf-8') as handle:
        json.dump(job, handle, ensure_ascii=False, indent=2)

    argv = [config['executable'], '--export-job', request_path]
    if config['validation']:
        argv.append('--validation')
    if progress is None:
        code, log = run(argv, timeout, timeout_code='native_timeout', cwd=work)
    else:
        reader = RenderProgressReader(frames_dir, config['frames'], clock=progress.clock)

        def on_tick():
            item = reader.sample()
            if item is not None:
                progress.note_render(item['completedFrames'],
                                     render_seconds=item.get('renderSeconds') or item.get('elapsedSeconds'),
                                     readback_seconds=item.get('readbackWriteSeconds'))

        code, log = _run_monitored(run, argv, timeout, timeout_code='native_timeout',
                                   cwd=work, on_tick=on_tick, progress=progress)
    if config['validation']:
        # Kept by the parent job's worker.log after temporary frames are removed.
        print(log, flush=True)

    result_path = os.path.join(frames_dir, 'result.json')
    if not os.path.isfile(result_path):
        raise ExportError('native_failed', f'原生导出没有生成 result.json（退出码 {code}）', log)
    try:
        native = json.loads(Path(result_path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise ExportError('native_failed', f'原生 result.json 无法解析：{error}', log) from error
    if not isinstance(native, dict) or native.get('ok') is not True:
        detail = native.get('error') if isinstance(native, dict) else None
        raise ExportError('native_failed',
                          '原生导出失败：' + (str(detail) if detail else f'退出码 {code}'), log)
    if code != 0:
        raise ExportError('native_failed', f'原生导出退出码 {code}，但 result.json 报告成功', log)
    return native


def build_ffmpeg_mp4(ffmpeg, frames_dir, fps, temp_out, *, progress_path=None):
    argv = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y']
    if progress_path:
        # 真实编码进度写进自己的临时文件，读者按块解析；不改变分辨率与编码参数。
        argv += ['-progress', str(progress_path)]
    return argv + ['-framerate', str(fps), '-start_number', '0',
                   '-i', os.path.join(frames_dir, FRAME_PATTERN),
                   '-an', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-r', str(fps),
                   '-movflags', '+faststart', temp_out]


def build_ffmpeg_gif(ffmpeg, frames_dir, fps, palette_path, temp_out, *,
                     palette_progress=None, encode_progress=None):
    pattern = os.path.join(frames_dir, FRAME_PATTERN)
    head = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y']
    def common(progress_path):
        argv = list(head)
        if progress_path:
            argv += ['-progress', str(progress_path)]
        return argv + ['-framerate', str(fps), '-start_number', '0', '-i', pattern]
    palette = common(palette_progress) + ['-vf', 'palettegen=stats_mode=diff', '-frames:v', '1', palette_path]
    encode = common(encode_progress) + ['-i', palette_path, '-lavfi', 'paletteuse=dither=sierra2_4a',
                                        '-loop', '0', temp_out]
    return palette, encode


def _reader_fraction(config, progress_path):
    """按 request 的 duration/frames 把 ffmpeg 的 out_time 折算成 pass 内比例。"""
    return EncodeProgressReader(progress_path, duration=config['duration'], frames=config['frames'])


def _run_encode(argv, timeout, run, progress, phase, passes, config, progress_path=None):
    """跑一个 ffmpeg pass，并把它的真实进度映射成 job 根的 encode 阶段。

    progress_path 为空表示这一 pass 报不出比例：palettegen 只输出一帧（实测
    ``-progress`` 里 out_time 停在第一帧），所以那里只发布“阶段=palettegen、
    比例未知”，绝不用一个假比例充数。
    """
    if progress is None:
        return run(argv, timeout, timeout_code='encode_timeout')
    progress.begin_encode(phase, passes=passes)
    reader = _reader_fraction(config, progress_path) if progress_path else None

    def on_tick():
        progress.note_encode(phase, reader.sample() if reader is not None else None)

    return _run_monitored(run, argv, timeout, timeout_code='encode_timeout', cwd=None,
                          on_tick=on_tick, progress=progress)


def encode_video(config, frames_dir, temp_root, run, progress=None):
    """把帧序列编码到临时文件，返回临时文件路径。"""
    fmt = config['format']
    ffmpeg = config['ffmpeg']
    timeout = _timeout(FFMPEG_TIMEOUT_BASE, FFMPEG_TIMEOUT_PER_FRAME,
                       FFMPEG_TIMEOUT_MAX, config['frames'])
    temp_out = os.path.join(temp_root, 'output.' + fmt)

    if fmt == 'mp4':
        progress_path = os.path.join(temp_root, 'ffmpeg-progress-encode.txt')
        code, log = _run_encode(build_ffmpeg_mp4(ffmpeg, frames_dir, config['fps'], temp_out,
                                                 progress_path=progress_path),
                                timeout, run, progress, 'encode', 1, config, progress_path)
        if code != 0:
            raise ExportError('ffmpeg_failed', f'ffmpeg 编码失败（退出码 {code}）', log)
    else:
        palette_path = os.path.join(temp_root, 'palette.png')
        encode_progress = os.path.join(temp_root, 'ffmpeg-progress-encode.txt')
        palette_argv, encode_argv = build_ffmpeg_gif(
            ffmpeg, frames_dir, config['fps'], palette_path, temp_out,
            encode_progress=encode_progress)
        code, log = _run_encode(palette_argv, timeout, run, progress, 'palettegen', 2, config)
        if code != 0:
            raise ExportError('ffmpeg_failed', f'ffmpeg 调色板生成失败（退出码 {code}）', log)
        if not os.path.isfile(palette_path):
            raise ExportError('ffmpeg_failed', 'ffmpeg 没有生成调色板文件')
        code, log = _run_encode(encode_argv, timeout, run, progress, 'encode', 2, config,
                                encode_progress)
        if code != 0:
            raise ExportError('ffmpeg_failed', f'ffmpeg GIF 编码失败（退出码 {code}）', log)

    if not os.path.isfile(temp_out) or os.path.getsize(temp_out) == 0:
        raise ExportError('ffmpeg_failed', 'ffmpeg 没有生成非空的输出文件')
    return temp_out


def write_manifest(frames_dir, config):
    manifest = {
        'ok': True,
        'format': 'png',
        'width': config['width'],
        'height': config['height'],
        'fps': config['fps'],
        'frames': config['frames'],
        'duration': config['frames'] / config['fps'],
        'requestedDuration': config['duration'],
    }
    with open(os.path.join(frames_dir, 'export.json'), 'w', encoding='utf-8') as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)


def commit_directory(frames_dir, target):
    if os.path.exists(target):
        raise ExportError('target_exists', f'提交时发现 PNG 目标已存在：{target}')
    try:
        # PNG 目标从不允许覆盖，用 os.rename：Windows 拒绝既存目标，避免竞态。
        os.rename(frames_dir, target)
    except FileExistsError as error:
        raise ExportError('target_exists', f'提交时发现 PNG 目标已存在：{target}', str(error)) from error
    except OSError as error:
        raise ExportError('commit_failed', f'无法把 PNG 目录移动到目标：{target}', str(error)) from error


def commit_file(temp_out, target, overwrite):
    if not overwrite and os.path.exists(target):
        raise ExportError('target_exists', f'提交时发现目标已存在且 overwrite=false：{target}')
    try:
        if overwrite:
            os.replace(temp_out, target)
        else:
            # Windows 的 os.rename 在目标已存在时抛 FileExistsError，因此不会覆盖
            # "检查之后、提交之前"才出现的文件（避免 check-then-commit 竞态）。
            os.rename(temp_out, target)
    except FileExistsError as error:
        raise ExportError('target_exists', f'提交时目标已存在且 overwrite=false：{target}', str(error)) from error
    except OSError as error:
        raise ExportError('commit_failed', f'无法原子替换目标：{target}', str(error)) from error


def _export_validated(config, run, progress, cleanup_path=None):
    """校验过的导出流程；progress 为 None 时行为与旧版完全一致。

    cleanup_path 非空时，先独占创建临时目录、在目录内写所有权标记，最后才在 job 根
    目录发布 cleanup.json（凭据只指向本任务真正创建的目录）：worker 被强杀时
    ``finally`` 不会执行，凭据留在盘上，由编辑器侧的 BackgroundJob 按同一套绑定清理。
    清理凭据只有“临时目录真的没了”才会被丢掉，删不干净就留给父进程。
    """
    parent = validate_target(config)
    if progress is not None:
        progress.set_stage('prepare', force=True)

    record = Path(cleanup_path) if cleanup_path is not None else None
    try:
        if record is None:
            temp_root = _make_temp_dir(TEMP_PREFIX, parent)
        else:
            temp_root = _make_recorded_temp_dir(parent, record, config['output'])
    except OSError as error:
        raise ExportError('temp_failed', f'无法在目标同卷创建临时目录：{parent}', str(error)) from error
    try:
        frames_dir = os.path.join(temp_root, 'frames')
        os.mkdir(frames_dir)
        run_native_export(config, frames_dir, run, progress)
        if progress is not None:
            # 渲染进程成功退出了，帧数到满；整体比例仍停在渲染段末尾。
            progress.render_complete()
        verify_frames(frames_dir, config['frames'], config['width'], config['height'])

        if config['format'] == 'png':
            write_manifest(frames_dir, config)
            if progress is not None:
                progress.set_stage('commit', overall=COMMIT_START, force=True)
            commit_directory(frames_dir, config['output'])
        else:
            temp_out = encode_video(config, frames_dir, temp_root, run, progress)
            if progress is not None:
                progress.set_stage('commit', overall=COMMIT_START, force=True)
            commit_file(temp_out, config['output'], config['overwrite'])

        if progress is not None:
            # 只有目标真的原子提交成功之后才是 100%。
            progress.done()

        return {
            'ok': True,
            'output': config['output'],
            'format': config['format'],
            'frames': config['frames'],
            'width': config['width'],
            'height': config['height'],
            'fps': config['fps'],
            'duration': config['frames'] / config['fps'],
        }
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)
        if record is not None and not os.path.isdir(temp_root):
            # 只有临时目录真的不在了才丢凭据；否则把它留给父进程清理。
            _drop_cleanup_record(record)


def export_animation(request, *, runner=None, which=None, progress_path=None):
    """执行一次导出；成功返回结果字典，失败抛 ExportError。

    ``progress_path`` 是 job 根目录里的 progress.json（编辑器 BackgroundJob 的
    运行目录）；给出时全程发布实时进度，失败还会留下 ``stage="failed"`` 的终态，
    并在同目录写下 cleanup.json 作为父进程的清理凭据（凭据在临时目录创建成功、
    所有权标记写完之后才发布）。
    """
    run = runner or _run_hidden
    find = which or shutil.which
    config = validate_request(request, find=find)
    progress = None
    record = None
    if progress_path is not None:
        progress_path = Path(progress_path)
        progress = ProgressPublisher(progress_path, config['frames'], config['format'])
        record = progress_path.with_name(CLEANUP_NAME)
    try:
        return _export_validated(config, run, progress, record)
    except ExportError as error:
        if progress is not None:
            progress.fail(error)
        raise
    except Exception as error:  # noqa: BLE001 - 内部错误也要留下终态，然后照常抛出
        if progress is not None:
            progress.fail(error)
        raise


def _run_job(input_path, progress_path=None):
    try:
        if input_path.stat().st_size > MAX_REQUEST_BYTES:
            raise ExportError('invalid_request', '请求文件过大')
        request = json.loads(input_path.read_text(encoding='utf-8'))
    except ExportError:
        raise
    except OSError as error:
        raise ExportError('invalid_request', f'无法读取请求文件：{input_path}', str(error)) from error
    except ValueError as error:
        raise ExportError('invalid_request', f'请求文件不是合法 JSON：{input_path}', str(error)) from error
    if progress_path is None:
        # job 根目录就是 --input request.json 所在目录（编辑器传的就是它的运行目录）。
        progress_path = input_path.parent / PROGRESS_NAME
    return export_animation(request, progress_path=progress_path)


def _write_result(path, result):
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    except OSError:
        return False
    return True


def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description='把冻结场景快照导出为 PNG/MP4/GIF')
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        result = _run_job(args.input)
    except ExportError as error:
        result = {'ok': False, 'error': error.to_json()}
    except Exception as error:  # noqa: BLE001 - worker 必须始终写结构化结果
        result = {'ok': False, 'error': {'code': 'internal', 'message': str(error)}}
    if not _write_result(args.output, result):
        return 2
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    raise SystemExit(main())
