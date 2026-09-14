"""用原生编辑器渲染冻结场景快照的帧，再封装成 PNG / MP4 / GIF。

流程::

    validate request -> 校验目标 -> 在目标同卷建临时目录
      -> 写 request 副本（加入 framesDirectory 字段）
      -> 原生 editor --export-job <request 副本> [--validation]
      -> 校验 result.json 以及 frame_%06d.png 的数量和尺寸
      -> PNG: 写 export.json 清单后把目录原子移动到目标
         MP4/GIF: ffmpeg 编码到临时文件后再提交到目标
      -> 无论成败都清理临时目录

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
import uuid

SUPPORTED_FORMATS = ('png', 'mp4', 'gif')
FRAME_PATTERN = 'frame_%06d.png'
FRAME_RE = re.compile(r'^frame_(\d{6})\.png$')

MIN_SIZE, MAX_SIZE = 16, 4096
MIN_FPS, MAX_FPS = 1, 60
MIN_FRAMES, MAX_FRAMES = 1, 3600
MAX_REQUEST_BYTES = 1 << 20

LOG_TAIL = 4000
NATIVE_TIMEOUT_BASE = 180.0
NATIVE_TIMEOUT_PER_FRAME = 2.0
NATIVE_TIMEOUT_MAX = 3600.0
FFMPEG_TIMEOUT_BASE = 60.0
FFMPEG_TIMEOUT_PER_FRAME = 0.5
FFMPEG_TIMEOUT_MAX = 1800.0


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


def run_native_export(config, frames_dir, run):
    """写 request 副本（含 framesDirectory）并调用原生 editor --export-job。"""
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
    code, log = run(argv, timeout, timeout_code='native_timeout', cwd=work)
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


def build_ffmpeg_mp4(ffmpeg, frames_dir, fps, temp_out):
    return [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
            '-framerate', str(fps), '-start_number', '0',
            '-i', os.path.join(frames_dir, FRAME_PATTERN),
            '-an', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-r', str(fps),
            '-movflags', '+faststart', temp_out]


def build_ffmpeg_gif(ffmpeg, frames_dir, fps, palette_path, temp_out):
    pattern = os.path.join(frames_dir, FRAME_PATTERN)
    common = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
              '-framerate', str(fps), '-start_number', '0', '-i', pattern]
    palette = common + ['-vf', 'palettegen=stats_mode=diff', '-frames:v', '1', palette_path]
    encode = common + ['-i', palette_path, '-lavfi', 'paletteuse=dither=sierra2_4a',
                       '-loop', '0', temp_out]
    return palette, encode


def encode_video(config, frames_dir, temp_root, run):
    """把帧序列编码到临时文件，返回临时文件路径。"""
    fmt = config['format']
    ffmpeg = config['ffmpeg']
    timeout = _timeout(FFMPEG_TIMEOUT_BASE, FFMPEG_TIMEOUT_PER_FRAME,
                       FFMPEG_TIMEOUT_MAX, config['frames'])
    temp_out = os.path.join(temp_root, 'output.' + fmt)

    if fmt == 'mp4':
        code, log = run(build_ffmpeg_mp4(ffmpeg, frames_dir, config['fps'], temp_out),
                        timeout, timeout_code='encode_timeout')
        if code != 0:
            raise ExportError('ffmpeg_failed', f'ffmpeg 编码失败（退出码 {code}）', log)
    else:
        palette_path = os.path.join(temp_root, 'palette.png')
        palette_argv, encode_argv = build_ffmpeg_gif(
            ffmpeg, frames_dir, config['fps'], palette_path, temp_out)
        code, log = run(palette_argv, timeout, timeout_code='encode_timeout')
        if code != 0:
            raise ExportError('ffmpeg_failed', f'ffmpeg 调色板生成失败（退出码 {code}）', log)
        if not os.path.isfile(palette_path):
            raise ExportError('ffmpeg_failed', 'ffmpeg 没有生成调色板文件')
        code, log = run(encode_argv, timeout, timeout_code='encode_timeout')
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


def export_animation(request, *, runner=None, which=None):
    """执行一次导出；成功返回结果字典，失败抛 ExportError。"""
    run = runner or _run_hidden
    find = which or shutil.which
    config = validate_request(request, find=find)
    parent = validate_target(config)

    try:
        temp_root = _make_temp_dir('physics-export-', parent)
    except OSError as error:
        raise ExportError('temp_failed', f'无法在目标同卷创建临时目录：{parent}', str(error)) from error
    try:
        frames_dir = os.path.join(temp_root, 'frames')
        os.mkdir(frames_dir)
        run_native_export(config, frames_dir, run)
        verify_frames(frames_dir, config['frames'], config['width'], config['height'])

        if config['format'] == 'png':
            write_manifest(frames_dir, config)
            commit_directory(frames_dir, config['output'])
        else:
            temp_out = encode_video(config, frames_dir, temp_root, run)
            commit_file(temp_out, config['output'], config['overwrite'])

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


def _run_job(input_path):
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
    return export_animation(request)


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
