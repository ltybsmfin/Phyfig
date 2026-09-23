"""export_animation 的单元测试。

全部用注入的假 runner（原生 editor / ffmpeg）与真实临时文件系统，不启动
pdflatex、ffmpeg 或原生程序。运行方式::

    python -m unittest tools.test_export_animation
    # 或在该目录下: python -m unittest test_export_animation
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import threading
import time
import unittest
import zlib
from unittest import mock

from export_animation import (CLEANUP_MARKER, CLEANUP_NAME, CLEANUP_TOKEN_RE,
                              CLEANUP_TMP_NAME, CLEANUP_VERSION, COMMIT_START,
                              ESTIMATE_WINDOW, ExportError, MAX_FRAMES,
                              NATIVE_TIMEOUT_BASE, NATIVE_TIMEOUT_MAX,
                              NATIVE_TIMEOUT_PER_FRAME, OWNERSHIP_MARKER,
                              OWNERSHIP_NAME, PROGRESS_NAME,
                              PROGRESS_TMP_NAME, PROGRESS_VERSION, TEMP_PREFIX,
                              EncodeProgressReader, ProgressPublisher,
                              RenderProgressReader, build_ffmpeg_gif,
                              build_ffmpeg_mp4, commit_file, export_animation,
                              main, run_native_export, validate_request,
                              validate_target, _export_validated, _make_temp_dir,
                              _make_recorded_temp_dir, _run_hidden, _run_job,
                              _timeout, _write_cleanup_record, _write_ownership_marker)


def _png_bytes(width, height):
    """只需要前 24 字节合法（IHDR），verify_frames 不解码像素。"""
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IEND', b'')


# 读者（RenderProgressReader._read_file 用 open(path, 'rb')）在 Windows 上不开
# FILE_SHARE_DELETE，所以它短暂占住目标文件的瞬间，os.replace 会以
# ERROR_SHARING_VIOLATION（WinError 32）失败。生产的 ProgressPublisher.flush 把这
# 当成“这一次没更新”，交给下一次 tick 重试，绝不因此让导出失败。假 native 没有
# 下一次 tick，就在本地按同一语义做短而有界的重试（最多 10 × 10 ms）；仍失败只算
# 丢掉这一次样本，绝不抛出——发布进度永远是尽力而为，测试不能因为读者恰好占住文件
# 而变红。
PUBLISH_ATTEMPTS = 10
PUBLISH_RETRY_DELAY = 0.01


def publish_worker_progress(path, payload, *, attempts=PUBLISH_ATTEMPTS,
                            delay=PUBLISH_RETRY_DELAY):
    """把一份完整 JSON 原子发布成 ``path``，返回这一次是否真的写成功。"""
    path = Path(path)
    temporary = path.with_name(PROGRESS_TMP_NAME)
    temporary.write_text(json.dumps(payload), encoding='utf-8')
    for attempt in range(attempts):
        try:
            os.replace(temporary, path)
            return True
        except OSError:
            if attempt + 1 < attempts:
                time.sleep(delay)
    try:
        temporary.unlink()
    except OSError:
        pass
    return False


class _Workspace(unittest.TestCase):
    def setUp(self):
        self.root = Path(_make_temp_dir('export-test-'))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.exe = self.root / 'editor.exe'
        self.exe.write_bytes(b'MZ')
        self.snapshot = self.root / 'snapshot.json'
        self.snapshot.write_text('{"scene": {}}', encoding='utf-8')

    def request(self, **overrides):
        request = {
            'executable': str(self.exe),
            'snapshot': str(self.snapshot),
            'output': str(self.root / 'out.png'),
            'format': 'png',
            'width': 64,
            'height': 32,
            'fps': 30,
            'frames': 30,
            'duration': 1.0,
            'ffmpeg': 'ffmpeg',
            'overwrite': False,
        }
        request.update(overrides)
        return request

    def temp_dirs(self):
        return [p for p in self.root.iterdir() if p.name.startswith('physics-export-')]


class FakeNative:
    def __init__(self, frames=30, width=64, height=32, ok=True, code=0,
                 write_result=True, size=None, raises=None):
        self.frames = frames
        self.width = width
        self.height = height
        self.ok = ok
        self.code = code
        self.write_result = write_result
        self.size = size
        self.raises = raises
        self.requests = []
        self.argv = []
        self.timeouts = []

    def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
        self.timeouts.append(timeout)
        self.argv.append(list(argv))
        if self.raises is not None:
            raise self.raises
        request = json.loads(
            Path(argv[argv.index('--export-job') + 1]).read_text(encoding='utf-8'))
        self.requests.append(request)
        frames_dir = request['framesDirectory']
        size = self.size or (self.width, self.height)
        for index in range(self.frames):
            Path(frames_dir, 'frame_%06d.png' % index).write_bytes(_png_bytes(*size))
        if self.write_result:
            Path(frames_dir, 'result.json').write_text(
                json.dumps({'ok': self.ok}), encoding='utf-8')
        return self.code, ''


class FakeFfmpeg:
    def __init__(self, code=0, log=''):
        self.code = code
        self.log = log
        self.calls = []

    def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
        self.calls.append(list(argv))
        if self.code == 0:
            Path(argv[-1]).write_bytes(b'encoded')
        return self.code, self.log


class FakePipeline:
    """按 argv 内容分派：带 --export-job 的给 native，其余给 ffmpeg。"""

    def __init__(self, native=None, ffmpeg=None):
        self.native = native or FakeNative()
        self.ffmpeg = ffmpeg or FakeFfmpeg()

    def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
        if '--export-job' in argv:
            return self.native(argv, timeout, timeout_code=timeout_code, cwd=cwd)
        return self.ffmpeg(argv, timeout, timeout_code=timeout_code, cwd=cwd)


class FakeClock:
    """可注入 ProgressPublisher 的确定性时钟。"""

    def __init__(self, now=0.0):
        self.now = now

    def __call__(self):
        return self.now

    def advance(self, seconds):
        self.now += seconds


class RecordingProgress(ProgressPublisher):
    """记录每次 flush 时的载荷，便于断言阶段语义而不只是最终文件。"""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.history = []

    def flush(self, *, force=False):
        if force or not self.history or self.payload() != self.history[-1]:
            self.history.append(self.payload())
        return super().flush(force=force)


class StreamingNative:
    """假 native：逐帧原子发布 framesDirectory/progress.json，并记录所在线程。

    模拟真实渲染 worker 的发布节奏与渲染时钟（renderSeconds 由假时钟推进），
    用来验证监测端把 native 进度映射成 job 根进度而不是等进程退出。发布方式和
    生产的 ``ProgressPublisher`` 一样是「写临时文件 + ``os.replace``」，读者占住
    目标文件时按同一契约短重试并放弃，绝不抛出（见 ``publish_worker_progress``）。
    """

    def __init__(self, frames=6, width=64, height=32, sample_delay=0.05,
                 render_seconds_step=0.5, thread=None):
        self.frames = frames
        self.width = width
        self.height = height
        self.sample_delay = sample_delay
        self.render_seconds_step = render_seconds_step
        self.threads = []
        self.argv = []
        self.requests = []
        self.publish_failures = 0

    def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
        self.threads.append(threading.current_thread())
        self.argv.append(list(argv))
        request = json.loads(
            Path(argv[argv.index('--export-job') + 1]).read_text(encoding='utf-8'))
        self.requests.append(request)
        frames_dir = Path(request['framesDirectory'])
        for index in range(self.frames):
            Path(frames_dir, 'frame_%06d.png' % index).write_bytes(
                _png_bytes(self.width, self.height))
            render_seconds = (index + 1) * self.render_seconds_step
            payload = {'version': PROGRESS_VERSION, 'stage': 'render',
                       'completedFrames': index + 1, 'totalFrames': self.frames,
                       'elapsedSeconds': render_seconds, 'renderSeconds': render_seconds,
                       'readbackWriteSeconds': 0.01}
            # 和生产 worker 一样：写临时文件再原子改名；读者占住目标文件时按
            # 生产契约重试（见 publish_worker_progress），绝不抛出。
            if not publish_worker_progress(frames_dir / PROGRESS_NAME, payload):
                self.publish_failures += 1
            time.sleep(self.sample_delay)
        Path(frames_dir, 'result.json').write_text(
            json.dumps({'ok': True}), encoding='utf-8')
        return 0, ''


class FakeFfmpegProgress(FakeFfmpeg):
    """假 ffmpeg：按 -progress 路径追加 key=value 块，模拟真实编码进度。"""

    def __init__(self, blocks=(0.2, 0.6, 1.0), code=0, log='', delay=0.05):
        super().__init__(code=code, log=log)
        self.blocks = blocks
        self.delay = delay
        self.progress_files = []

    def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
        self.calls.append(list(argv))
        if '-progress' in argv:
            path = Path(argv[argv.index('-progress') + 1])
            self.progress_files.append(path)
            with path.open('w', encoding='utf-8') as handle:
                for fraction in self.blocks:
                    handle.write(f'frame={int(fraction * 30)}\n')
                    handle.write(f'out_time_us={int(fraction * 1_000_000)}\n')
                    handle.write(f'speed={fraction + 1:.2f}x\n')
                    handle.write('progress=continue\n')
                    handle.flush()
                    time.sleep(self.delay)
                handle.write('progress=end\n')
                handle.flush()
        if self.code == 0:
            Path(argv[-1]).write_bytes(b'encoded')
        return self.code, self.log


class ValidateRequestTest(_Workspace):
    def test_accepts_png(self):
        config = validate_request(self.request())
        self.assertEqual(config['format'], 'png')
        self.assertEqual((config['width'], config['height']), (64, 32))
        self.assertIsNone(config['ffmpeg'])

    def test_accepts_mp4_and_resolves_ffmpeg(self):
        config = validate_request(self.request(format='mp4', output=str(self.root / 'out.mp4')),
                                  find=lambda name: str(self.exe))
        self.assertEqual(config['ffmpeg'], str(self.exe))

    def test_accepts_integral_float_dimensions(self):
        config = validate_request(self.request(width=64.0, height=32.0))
        self.assertEqual((config['width'], config['height']), (64, 32))

    def test_static_single_frame(self):
        config = validate_request(self.request(fps=60, frames=1, duration=1.0 / 60))
        self.assertEqual(config['frames'], 1)

    def test_single_frame_beyond_one_period_rejected(self):
        with self.assertRaises(ExportError) as context:
            validate_request(self.request(fps=60, frames=1, duration=0.02))
        self.assertEqual(context.exception.code, 'invalid_request')

    def test_rejects_odd_size(self):
        with self.assertRaises(ExportError) as context:
            validate_request(self.request(width=65))
        self.assertEqual(context.exception.code, 'invalid_request')

    def test_rejects_size_range(self):
        for width, height in ((14, 32), (64, 4098), (0, 0)):
            with self.subTest(width=width, height=height), self.assertRaises(ExportError):
                validate_request(self.request(width=width, height=height))

    def test_rejects_bool_as_int(self):
        with self.assertRaises(ExportError):
            validate_request(self.request(width=True))

    def test_rejects_fps_range(self):
        for fps in (0, 61):
            with self.subTest(fps=fps), self.assertRaises(ExportError):
                validate_request(self.request(fps=fps, duration=1.0, frames=fps))

    def test_rejects_frames_not_ceiling(self):
        with self.assertRaises(ExportError) as context:
            validate_request(self.request(fps=30, duration=1.0, frames=29))
        self.assertIn('ceil', str(context.exception))

    def test_rejects_more_than_3600_frames(self):
        with self.assertRaises(ExportError) as context:
            validate_request(self.request(fps=60, duration=120.0, frames=3600))
        self.assertIn(str(MAX_FRAMES), str(context.exception))

    def test_rejects_bad_duration(self):
        for duration in (0, -1, float('nan'), float('inf'), 'x'):
            with self.subTest(duration=duration), self.assertRaises(ExportError):
                validate_request(self.request(duration=duration, frames=30))

    def test_rejects_relative_paths(self):
        for key in ('executable', 'snapshot', 'output'):
            with self.subTest(key=key), self.assertRaises(ExportError):
                validate_request(self.request(**{key: 'relative/path'}))

    def test_rejects_missing_files(self):
        with self.assertRaises(ExportError):
            validate_request(self.request(executable=str(self.root / 'nope.exe')))
        with self.assertRaises(ExportError):
            validate_request(self.request(snapshot=str(self.root / 'nope.json')))

    def test_rejects_bad_format(self):
        for fmt in ('webm', '', 3, None):
            with self.subTest(fmt=fmt), self.assertRaises(ExportError):
                validate_request(self.request(format=fmt))

    def test_rejects_non_bool_overwrite(self):
        with self.assertRaises(ExportError):
            validate_request(self.request(overwrite='yes'))

    def test_validation_defaults_to_false(self):
        self.assertFalse(validate_request(self.request())['validation'])

    def test_accepts_validation_true(self):
        self.assertTrue(validate_request(self.request(validation=True))['validation'])

    def test_rejects_non_bool_validation(self):
        with self.assertRaises(ExportError):
            validate_request(self.request(validation='yes'))

    def test_rejects_missing_ffmpeg(self):
        with self.assertRaises(ExportError) as context:
            validate_request(self.request(format='mp4', output=str(self.root / 'out.mp4')),
                             find=lambda name: None)
        self.assertEqual(context.exception.code, 'ffmpeg_missing')


class ValidateTargetTest(_Workspace):
    def _config(self, **overrides):
        return validate_request(self.request(**overrides), find=lambda name: str(self.exe))

    def test_png_existing_directory_rejected(self):
        target = self.root / 'out'
        target.mkdir()
        with self.assertRaises(ExportError) as context:
            validate_target(self._config(output=str(target)))
        self.assertEqual(context.exception.code, 'target_exists')

    def test_png_existing_directory_rejected_even_with_overwrite(self):
        target = self.root / 'out'
        target.mkdir()
        with self.assertRaises(ExportError) as context:
            validate_target(self._config(output=str(target), overwrite=True))
        self.assertEqual(context.exception.code, 'target_exists')

    def test_missing_parent_rejected(self):
        with self.assertRaises(ExportError) as context:
            validate_target(self._config(output=str(self.root / 'nope' / 'out')))
        self.assertEqual(context.exception.code, 'not_found')

    def test_video_extension_must_match(self):
        with self.assertRaises(ExportError) as context:
            validate_target(self._config(format='mp4', output=str(self.root / 'out.gif')))
        self.assertEqual(context.exception.code, 'invalid_request')

    def test_video_existing_without_overwrite_rejected(self):
        target = self.root / 'out.mp4'
        target.write_bytes(b'old')
        with self.assertRaises(ExportError) as context:
            validate_target(self._config(format='mp4', output=str(target)))
        self.assertEqual(context.exception.code, 'target_exists')

    def test_video_existing_with_overwrite_allowed(self):
        target = self.root / 'out.mp4'
        target.write_bytes(b'old')
        parent = validate_target(self._config(format='mp4', output=str(target), overwrite=True))
        self.assertEqual(parent, str(self.root))

    def test_video_target_directory_rejected(self):
        target = self.root / 'out.mp4'
        target.mkdir()
        with self.assertRaises(ExportError) as context:
            validate_target(self._config(format='mp4', output=str(target), overwrite=True))
        self.assertEqual(context.exception.code, 'invalid_request')


class FfmpegArgsTest(_Workspace):
    def test_mp4_args(self):
        argv = build_ffmpeg_mp4('ffmpeg.exe', 'C:/frames', 30, 'C:/tmp/out.mp4')
        self.assertEqual(argv[0], 'ffmpeg.exe')
        self.assertIn('-nostdin', argv)
        self.assertIn('-y', argv)
        self.assertEqual(argv[argv.index('-framerate') + 1], '30')
        self.assertEqual(argv[argv.index('-start_number') + 1], '0')
        self.assertTrue(argv[argv.index('-i') + 1].endswith('frame_%06d.png'))
        self.assertEqual(argv[-1], 'C:/tmp/out.mp4')

    def test_mp4_never_scales(self):
        argv = build_ffmpeg_mp4('ffmpeg.exe', 'C:/frames', 30, 'C:/tmp/out.mp4')
        joined = ' '.join(argv).lower()
        self.assertNotIn('scale', joined)
        self.assertNotIn('-s ', joined + ' ')

    def test_gif_uses_palettegen_and_paletteuse(self):
        palette, encode = build_ffmpeg_gif(
            'ffmpeg.exe', 'C:/frames', 30, 'C:/tmp/palette.png', 'C:/tmp/out.gif')
        self.assertIn('palettegen=stats_mode=diff', palette)
        self.assertIn('paletteuse=dither=sierra2_4a', encode)
        self.assertEqual(encode[encode.index('-loop') + 1], '0')
        for argv in (palette, encode):
            self.assertIn('-nostdin', argv)
            self.assertIn('-y', argv)
        self.assertEqual(palette[-1], 'C:/tmp/palette.png')
        self.assertEqual(encode[-1], 'C:/tmp/out.gif')

    def test_y_is_only_used_on_owned_temp_files(self):
        target = 'C:/final/out.mp4'
        argv = build_ffmpeg_mp4('ffmpeg.exe', 'C:/frames', 30, 'C:/tmp/out.mp4')
        self.assertNotEqual(argv[-1], target)
        self.assertEqual(argv.count('-y'), 1)


class ExportFlowTest(_Workspace):
    def test_png_success(self):
        native = FakeNative(frames=30)
        request = self.request(output=str(self.root / 'out'), custom={'a': 1})
        result = export_animation(request, runner=native)
        self.assertTrue(result['ok'])
        self.assertEqual(result['format'], 'png')
        self.assertEqual(result['frames'], 30)
        self.assertEqual(result['duration'], 1.0)
        output = self.root / 'out'
        self.assertTrue(output.is_dir())
        self.assertTrue((output / 'frame_000000.png').is_file())
        self.assertTrue((output / 'frame_000029.png').is_file())
        manifest = json.loads((output / 'export.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['width'], 64)
        self.assertEqual(manifest['fps'], 30)
        self.assertEqual(manifest['frames'], 30)
        self.assertAlmostEqual(manifest['duration'], 1.0)
        job = native.requests[0]
        self.assertTrue(job['framesDirectory'].endswith('frames'))
        self.assertEqual(job['snapshot'], str(self.snapshot))
        self.assertEqual(job['custom'], {'a': 1})
        self.assertEqual(native.timeouts[0],
                         _timeout(NATIVE_TIMEOUT_BASE, NATIVE_TIMEOUT_PER_FRAME,
                                  NATIVE_TIMEOUT_MAX, 30))
        self.assertEqual(self.temp_dirs(), [])

    def test_native_has_no_validation_flag_by_default(self):
        native = FakeNative(frames=30)
        export_animation(self.request(output=str(self.root / 'out')), runner=native)
        self.assertNotIn('--validation', native.argv[0])

    def test_native_gets_validation_flag_when_requested(self):
        native = FakeNative(frames=30)
        export_animation(self.request(output=str(self.root / 'out'), validation=True),
                         runner=native)
        self.assertIn('--validation', native.argv[0])
        self.assertIs(native.requests[0]['validation'], True)

    def test_mp4_success(self):
        pipeline = FakePipeline(native=FakeNative(frames=30))
        result = export_animation(
            self.request(format='mp4', output=str(self.root / 'out.mp4')),
            runner=pipeline, which=lambda name: str(self.exe))
        self.assertTrue(result['ok'])
        self.assertEqual(result['format'], 'mp4')
        self.assertEqual(len(pipeline.ffmpeg.calls), 1)
        self.assertEqual((self.root / 'out.mp4').read_bytes(), b'encoded')
        self.assertEqual(self.temp_dirs(), [])

    def test_gif_success_two_passes(self):
        pipeline = FakePipeline(native=FakeNative(frames=30))
        result = export_animation(
            self.request(format='gif', output=str(self.root / 'out.gif')),
            runner=pipeline, which=lambda name: str(self.exe))
        self.assertTrue(result['ok'])
        self.assertEqual(len(pipeline.ffmpeg.calls), 2)
        self.assertIn('palettegen=stats_mode=diff', pipeline.ffmpeg.calls[0])
        self.assertIn('paletteuse=dither=sierra2_4a', pipeline.ffmpeg.calls[1])
        self.assertEqual((self.root / 'out.gif').read_bytes(), b'encoded')

    def test_overwrite_true_replaces_existing_target(self):
        target = self.root / 'out.mp4'
        target.write_bytes(b'old')
        pipeline = FakePipeline(native=FakeNative(frames=30))
        result = export_animation(
            self.request(format='mp4', output=str(target), overwrite=True),
            runner=pipeline, which=lambda name: str(self.exe))
        self.assertTrue(result['ok'])
        self.assertEqual(target.read_bytes(), b'encoded')

    def test_overwrite_false_rejects_before_native(self):
        target = self.root / 'out.mp4'
        target.write_bytes(b'old')
        pipeline = FakePipeline(native=FakeNative(frames=30))
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(format='mp4', output=str(target), overwrite=False),
                             runner=pipeline, which=lambda name: str(self.exe))
        self.assertEqual(context.exception.code, 'target_exists')
        self.assertEqual(target.read_bytes(), b'old')
        self.assertEqual(pipeline.native.requests, [])
        self.assertEqual(self.temp_dirs(), [])

    def test_commit_race_without_overwrite_keeps_intruder_file(self):
        target = self.root / 'out.mp4'

        class RacingFfmpeg(FakeFfmpeg):
            def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
                target.write_bytes(b'intruder')  # 检查之后、提交之前才出现的目标
                return super().__call__(argv, timeout, timeout_code=timeout_code, cwd=cwd)

        pipeline = FakePipeline(native=FakeNative(frames=30), ffmpeg=RacingFfmpeg())
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(format='mp4', output=str(target), overwrite=False),
                             runner=pipeline, which=lambda name: str(self.exe))
        self.assertEqual(context.exception.code, 'target_exists')
        self.assertEqual(target.read_bytes(), b'intruder')
        self.assertEqual(self.temp_dirs(), [])

    def test_native_failure_keeps_target_and_cleans_temp(self):
        native = FakeNative(frames=30, ok=False)
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(output=str(self.root / 'out')), runner=native)
        self.assertEqual(context.exception.code, 'native_failed')
        self.assertFalse((self.root / 'out').exists())
        self.assertEqual(self.temp_dirs(), [])

    def test_native_missing_result(self):
        native = FakeNative(frames=30, write_result=False)
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(), runner=native)
        self.assertEqual(context.exception.code, 'native_failed')

    def test_native_timeout_propagates(self):
        native = FakeNative(raises=ExportError('native_timeout', 'boom', 'log tail'))
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(output=str(self.root / 'out')), runner=native)
        self.assertEqual(context.exception.code, 'native_timeout')
        self.assertFalse((self.root / 'out').exists())
        self.assertEqual(self.temp_dirs(), [])

    def test_missing_frame(self):
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(), runner=FakeNative(frames=29))
        self.assertEqual(context.exception.code, 'frame_missing')

    def test_extra_frame(self):
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(), runner=FakeNative(frames=31))
        self.assertEqual(context.exception.code, 'frame_unexpected')

    def test_frame_size_mismatch(self):
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(), runner=FakeNative(frames=30, size=(32, 32)))
        self.assertEqual(context.exception.code, 'frame_size_mismatch')

    def test_ffmpeg_failed(self):
        pipeline = FakePipeline(native=FakeNative(frames=30), ffmpeg=FakeFfmpeg(code=1, log='boom'))
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(format='mp4', output=str(self.root / 'out.mp4')),
                             runner=pipeline, which=lambda name: str(self.exe))
        self.assertEqual(context.exception.code, 'ffmpeg_failed')
        self.assertIn('boom', context.exception.detail or '')
        self.assertFalse((self.root / 'out.mp4').exists())

    def test_ffmpeg_missing(self):
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(format='mp4', output=str(self.root / 'out.mp4')),
                             runner=FakePipeline(), which=lambda name: None)
        self.assertEqual(context.exception.code, 'ffmpeg_missing')

    def test_encode_writes_only_temp_then_commits(self):
        target = self.root / 'out.mp4'
        seen = {}

        class RecordingFfmpeg(FakeFfmpeg):
            def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
                seen['argv'] = list(argv)
                seen['target_exists_during_encode'] = target.exists()
                seen['output'] = argv[-1]
                return super().__call__(argv, timeout, timeout_code=timeout_code, cwd=cwd)

        pipeline = FakePipeline(native=FakeNative(frames=30), ffmpeg=RecordingFfmpeg())
        export_animation(self.request(format='mp4', output=str(target)),
                         runner=pipeline, which=lambda name: str(self.exe))
        self.assertNotEqual(seen['output'], str(target))
        self.assertFalse(seen['target_exists_during_encode'])
        self.assertTrue(target.is_file())


class CommitFileTest(_Workspace):
    def test_rename_used_when_not_overwriting(self):
        temp = self.root / 'tmp.mp4'
        temp.write_bytes(b'data')
        target = str(self.root / 'out.mp4')
        with mock.patch('export_animation.os.rename') as rename:
            commit_file(str(temp), target, False)
        rename.assert_called_once_with(str(temp), target)

    def test_replace_used_when_overwriting(self):
        temp = self.root / 'tmp.mp4'
        temp.write_bytes(b'data')
        target = str(self.root / 'out.mp4')
        with mock.patch('export_animation.os.replace') as replace:
            commit_file(str(temp), target, True)
        replace.assert_called_once_with(str(temp), target)

    def test_file_exists_maps_to_target_exists(self):
        temp = self.root / 'tmp.mp4'
        temp.write_bytes(b'data')
        with mock.patch('export_animation.os.rename', side_effect=FileExistsError('exists')):
            with self.assertRaises(ExportError) as context:
                commit_file(str(temp), str(self.root / 'out.mp4'), False)
        self.assertEqual(context.exception.code, 'target_exists')

    def test_existing_target_rejected_before_rename(self):
        temp = self.root / 'tmp.mp4'
        temp.write_bytes(b'data')
        target = self.root / 'out.mp4'
        target.write_bytes(b'old')
        with mock.patch('export_animation.os.rename') as rename:
            with self.assertRaises(ExportError) as context:
                commit_file(str(temp), str(target), False)
        self.assertEqual(context.exception.code, 'target_exists')
        rename.assert_not_called()


class RunHiddenTest(unittest.TestCase):
    def test_returns_code_and_truncated_log(self):
        process = mock.Mock(returncode=0)
        process.communicate.return_value = (b'hello\n', b'')
        with mock.patch('export_animation.subprocess.Popen', return_value=process) as popen:
            code, log = _run_hidden(['tool', '--flag'], 5)
        self.assertEqual((code, log), (0, 'hello'))
        args, kwargs = popen.call_args
        self.assertEqual(args[0], ['tool', '--flag'])
        self.assertFalse(kwargs['shell'])
        self.assertEqual(kwargs['stdin'], subprocess.DEVNULL)

    def test_timeout_kills_direct_child(self):
        process = mock.Mock()
        process.communicate.side_effect = [
            subprocess.TimeoutExpired('tool', 1),
            (b'tail', b''),
        ]
        with mock.patch('export_animation.subprocess.Popen', return_value=process):
            with self.assertRaises(ExportError) as context:
                _run_hidden(['tool'], 1, timeout_code='native_timeout')
        self.assertEqual(context.exception.code, 'native_timeout')
        process.kill.assert_called_once()

    def test_log_is_truncated(self):
        process = mock.Mock(returncode=1)
        process.communicate.return_value = (b'x' * 10000, b'')
        with mock.patch('export_animation.subprocess.Popen', return_value=process):
            code, log = _run_hidden(['tool'], 5)
        self.assertEqual(code, 1)
        self.assertLessEqual(len(log), 4003)
        self.assertTrue(log.startswith('...'))


class ErrorShapeTest(unittest.TestCase):
    def test_to_json(self):
        error = ExportError('native_failed', '失败了', 'detail')
        self.assertEqual(error.to_json(),
                         {'code': 'native_failed', 'message': '失败了', 'detail': 'detail'})
        self.assertEqual(ExportError('x', 'y').to_json(), {'code': 'x', 'message': 'y'})


class MainTest(_Workspace):
    def test_invalid_request_writes_structured_error(self):
        request_path = self.root / 'request.json'
        request_path.write_text(json.dumps({'format': 'png'}), encoding='utf-8')
        result_path = self.root / 'result.json'
        code = main(['--input', str(request_path), '--output', str(result_path)])
        self.assertEqual(code, 1)
        result = json.loads(result_path.read_text(encoding='utf-8'))
        self.assertFalse(result['ok'])
        self.assertIn('code', result['error'])
        self.assertIn('message', result['error'])

    def test_success_writes_result(self):
        request_path = self.root / 'request.json'
        request_path.write_text(json.dumps(self.request()), encoding='utf-8')
        result_path = self.root / 'result.json'
        payload = {'ok': True, 'output': str(self.root / 'out'), 'format': 'png',
                   'frames': 30, 'width': 64, 'height': 32, 'fps': 30, 'duration': 1.0}
        with mock.patch('export_animation.export_animation', return_value=payload):
            code = main(['--input', str(request_path), '--output', str(result_path)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(result_path.read_text(encoding='utf-8')), payload)

    def test_internal_error_is_reported_not_raised(self):
        request_path = self.root / 'request.json'
        request_path.write_text(json.dumps(self.request()), encoding='utf-8')
        result_path = self.root / 'result.json'
        with mock.patch('export_animation._run_job', side_effect=RuntimeError('boom')):
            code = main(['--input', str(request_path), '--output', str(result_path)])
        self.assertEqual(code, 1)
        result = json.loads(result_path.read_text(encoding='utf-8'))
        self.assertEqual(result['error']['code'], 'internal')
        self.assertIn('boom', result['error']['message'])

    def test_oversized_request_rejected(self):
        request_path = self.root / 'request.json'
        request_path.write_bytes(b'x' * ((1 << 20) + 1))
        result_path = self.root / 'result.json'
        code = main(['--input', str(request_path), '--output', str(result_path)])
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(result_path.read_text(encoding='utf-8'))['error']['code'],
                         'invalid_request')


class ProgressPublisherTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(_make_temp_dir('export-progress-'))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.clock = FakeClock()
        self.path = self.root / PROGRESS_NAME

    def publisher(self, *, total=8, fmt='mp4'):
        return ProgressPublisher(self.path, total, fmt, clock=self.clock)

    def read(self):
        return json.loads(self.path.read_text(encoding='utf-8'))

    def test_prepare_is_published_first_with_zero_frames(self):
        publisher = self.publisher()
        publisher.set_stage('prepare', force=True)
        payload = self.read()
        self.assertEqual(payload['version'], PROGRESS_VERSION)
        self.assertEqual(payload['stage'], 'prepare')
        self.assertEqual(payload['completedFrames'], 0)
        self.assertEqual(payload['totalFrames'], 8)
        self.assertEqual(payload['overallFraction'], 0.0)
        self.assertLessEqual(payload['elapsedSeconds'], 0.001)

    def test_render_is_monotonic_and_clamped_to_total(self):
        publisher = RecordingProgress(self.path, 8, 'mp4', clock=self.clock)
        for completed in (1, 3, 3, 99, 0):
            self.clock.advance(0.3)
            publisher.note_render(completed)
        payload = self.read()
        self.assertEqual(payload['stage'], 'render')
        self.assertEqual(payload['completedFrames'], 8)
        self.assertLessEqual(payload['overallFraction'], 0.90)
        history = [item['completedFrames'] for item in publisher.history]
        self.assertEqual(history, sorted(history))
        self.assertEqual(history[-1], 8)

    def test_render_complete_is_not_export_complete(self):
        for fmt, ceiling in (('png', 0.98), ('mp4', 0.90), ('gif', 0.90)):
            with self.subTest(fmt=fmt):
                clock = FakeClock()
                path = self.root / f'{fmt}.json'
                publisher = ProgressPublisher(path, 4, fmt, clock=clock)
                publisher.note_render(4)
                publisher.render_complete()
                payload = json.loads(path.read_text(encoding='utf-8'))
                self.assertEqual(payload['stage'], 'render')
                self.assertEqual(payload['completedFrames'], 4)
                self.assertAlmostEqual(payload['overallFraction'], ceiling)
                self.assertLess(payload['overallFraction'], 1.0)
                publisher.done()
                payload = json.loads(path.read_text(encoding='utf-8'))
                self.assertEqual(payload['stage'], 'done')
                self.assertEqual(payload['overallFraction'], 1.0)

    def test_failure_keeps_the_last_fraction_and_never_claims_100(self):
        publisher = self.publisher(total=8)
        self.clock.advance(0.3)
        publisher.note_render(4)
        self.clock.advance(ESTIMATE_WINDOW)
        publisher.note_render(6)
        self.assertIsNotNone(self.read()['fps'])
        publisher.fail(ExportError('ffmpeg_failed', 'boom'))
        payload = self.read()
        self.assertEqual(payload['stage'], 'failed')
        self.assertEqual(payload['completedFrames'], 6)
        self.assertLess(payload['overallFraction'], 1.0)
        self.assertEqual(payload['message'], 'boom')
        # 任务结束后不再推进：过期吞吐和预计剩余都不能留在终态载荷里，否则终态字段
        # 会随“最后一次发布是否被读到”而变（编辑器会自己合成同形的终态）。
        self.assertIsNone(payload['fps'])
        self.assertIsNone(payload['estimatedRemainingSeconds'])
        self.assertIsNone(payload['estimateScope'])
        self.assertIsNone(payload['phaseFraction'])

    def test_estimate_stays_null_until_a_stable_window(self):
        publisher = self.publisher(total=10)
        publisher.note_render(1)
        payload = self.read()
        self.assertIsNone(payload['fps'])
        self.assertIsNone(payload['estimatedRemainingSeconds'])
        self.assertEqual(payload['estimateScope'], 'render')
        self.clock.advance(ESTIMATE_WINDOW + 0.1)
        publisher.note_render(3)
        payload = self.read()
        self.assertIsNotNone(payload['fps'])
        self.assertAlmostEqual(payload['fps'], 2.0 / (ESTIMATE_WINDOW + 0.1), places=3)
        self.assertIsNotNone(payload['estimatedRemainingSeconds'])

    def test_render_worker_clock_is_a_valid_estimate_source(self):
        publisher = self.publisher(total=10)
        publisher.note_render(4, render_seconds=2.0)
        payload = self.read()
        self.assertAlmostEqual(payload['fps'], 2.0)
        self.assertAlmostEqual(payload['estimatedRemainingSeconds'], 3.0)
        self.assertEqual(payload['renderSeconds'], 2.0)

    def test_writes_are_throttled_until_the_heartbeat(self):
        publisher = self.publisher(total=10)
        publisher.set_stage('prepare', force=True)
        self.assertEqual(publisher.writes, 1)
        for completed in range(1, 11):
            self.clock.advance(0.01)
            publisher.note_render(completed)
        self.assertEqual(publisher.writes, 1, '0.1 s 内的多次变化必须被节流')
        self.clock.advance(0.2)
        publisher.note_render(10)
        self.assertEqual(publisher.writes, 2)
        # 值不变时最多按心跳刷新，不会每次调用都写盘。
        for _ in range(20):
            self.clock.advance(0.01)
            publisher.flush()
        self.assertEqual(publisher.writes, 2)
        self.clock.advance(1.0)
        publisher.flush()
        self.assertEqual(publisher.writes, 3)

    def test_phase_change_is_published_immediately(self):
        publisher = self.publisher(total=4, fmt='gif')
        publisher.note_render(4)
        publisher.render_complete()
        publisher.begin_encode('palettegen', passes=2)
        payload = self.read()
        self.assertEqual(payload['stage'], 'encode')
        self.assertEqual(payload['phase'], 'palettegen')
        self.assertIsNone(payload['phaseFraction'])
        self.assertAlmostEqual(payload['overallFraction'], 0.90)

    def test_two_pass_encode_maps_both_passes_into_the_encode_span(self):
        publisher = self.publisher(total=4, fmt='gif')
        publisher.note_render(4)
        publisher.render_complete()
        publisher.begin_encode('palettegen', passes=2)
        self.clock.advance(0.2)
        publisher.note_encode('palettegen', None)
        first = self.read()
        self.assertAlmostEqual(first['overallFraction'], 0.90)
        self.assertIsNone(first['phaseFraction'])
        publisher.begin_encode('encode', passes=2)
        self.clock.advance(0.2)
        publisher.note_encode('encode', 0.5)
        second = self.read()
        self.assertAlmostEqual(second['overallFraction'], 0.90 + (COMMIT_START - 0.90) * 0.75)
        self.assertGreater(second['overallFraction'], first['overallFraction'])
        self.assertLess(second['overallFraction'], 1.0)
        self.assertEqual(second['phase'], 'encode')
        self.assertAlmostEqual(second['phaseFraction'], 0.5)

    def test_single_pass_encode_uses_the_whole_encode_span(self):
        publisher = self.publisher(total=4, fmt='mp4')
        publisher.note_render(4)
        publisher.render_complete()
        publisher.begin_encode('encode', passes=1)
        self.clock.advance(0.2)
        publisher.note_encode('encode', 0.25)
        payload = self.read()
        expected = 0.90 + (COMMIT_START - 0.90) * 0.25
        self.assertAlmostEqual(payload['overallFraction'], expected)
        self.assertAlmostEqual(payload['phaseFraction'], 0.25)

    def test_publish_is_atomic_and_leaves_no_temp_file(self):
        publisher = self.publisher(total=4)
        publisher.set_stage('prepare', force=True)
        self.assertTrue(self.path.is_file())
        self.assertFalse((self.root / PROGRESS_TMP_NAME).exists())
        # 一次残缺的旧文件绝不可能被读者看到：每次替换都是完整 JSON。
        self.assertEqual(json.loads(self.path.read_text(encoding='utf-8'))['version'],
                         PROGRESS_VERSION)

    def test_publish_failure_is_counted_but_never_raises(self):
        blocked = self.root / 'blocked'
        blocked.mkdir()
        publisher = ProgressPublisher(blocked, 4, 'png', clock=self.clock)
        self.assertFalse(publisher.flush(force=True))
        self.assertEqual(publisher.writes, 0)
        self.assertEqual(publisher.failures, 1)


class RenderProgressReaderTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(_make_temp_dir('export-render-'))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.frames = self.root / 'frames'
        self.frames.mkdir()
        self.clock = FakeClock()

    def reader(self, total=10, **kwargs):
        return RenderProgressReader(self.frames, total, clock=self.clock, **kwargs)

    def write_worker_progress(self, payload):
        (self.frames / PROGRESS_NAME).write_text(json.dumps(payload), encoding='utf-8')

    def test_reads_the_render_worker_file(self):
        self.write_worker_progress({'version': PROGRESS_VERSION, 'stage': 'render',
                                    'completedFrames': 3, 'totalFrames': 10,
                                    'elapsedSeconds': 1.5, 'renderSeconds': 1.4,
                                    'readbackWriteSeconds': 0.1})
        sample = self.reader().sample()
        self.assertEqual(sample['completedFrames'], 3)
        self.assertEqual(sample['renderSeconds'], 1.4)
        self.assertEqual(sample['source'], 'worker')

    def test_corrupt_or_wrong_version_files_fall_back_to_frame_counting(self):
        for index, text in enumerate(('{not json', '{"version":2,"stage":"render","completedFrames":5}',
                                      '{"version":1,"completedFrames":5}',
                                      '{"version":1,"stage":"render","completedFrames":5,"totalFrames":99}')):
            with self.subTest(text=text):
                self.frames.joinpath('frame_%06d.png' % index).write_bytes(b'x')
                self.frames.joinpath(PROGRESS_NAME).write_text(text, encoding='utf-8')
                reader = self.reader()
                sample = reader.sample()
                self.assertIsNotNone(sample)
                self.assertEqual(sample['source'], 'frames')
                self.assertEqual(sample['completedFrames'], index + 1)
                self.clock.advance(1.0)

    def test_oversized_and_empty_progress_files_are_ignored(self):
        (self.frames / PROGRESS_NAME).write_bytes(b' ' * (64 * 1024 + 8))
        self.frames.joinpath('frame_000000.png').write_bytes(b'x')
        self.assertEqual(self.reader().sample()['source'], 'frames')
        (self.frames / PROGRESS_NAME).write_bytes(b'')
        self.clock.advance(1.0)
        self.assertEqual(self.reader().sample()['source'], 'frames')

    def test_frame_counting_is_monotonic_and_throttled(self):
        for index in range(3):
            self.frames.joinpath('frame_%06d.png' % index).write_bytes(b'x')
        reader = self.reader(total=10)
        self.assertEqual(reader.sample()['completedFrames'], 3)
        # 扫描被节流：间隔不够时不重新读目录。
        self.assertIsNone(reader.sample())
        self.clock.advance(1.0)
        self.frames.joinpath('frame_000003.png').write_bytes(b'x')
        self.assertEqual(reader.sample()['completedFrames'], 4)
        os_removed = self.frames / 'frame_000003.png'
        os_removed.unlink()
        self.clock.advance(1.0)
        self.assertEqual(reader.sample()['completedFrames'], 4, '帧数不允许倒退')

    def test_missing_directory_yields_no_sample(self):
        reader = RenderProgressReader(self.root / 'nope', 10, clock=self.clock)
        self.assertIsNone(reader.sample())


class EncodeProgressReaderTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(_make_temp_dir('export-encode-'))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.path = self.root / 'ffmpeg-progress.txt'

    def reader(self, duration=2.0, frames=48):
        return EncodeProgressReader(self.path, duration=duration, frames=frames)

    def test_reads_the_last_complete_block(self):
        self.path.write_text(
            'frame=1\nout_time_us=250000\nprogress=continue\n'
            'frame=2\nout_time_us=1000000\nprogress=continue\n', encoding='utf-8')
        self.assertAlmostEqual(self.reader().sample(), 0.5)

    def test_partial_trailing_block_is_not_preferred(self):
        self.path.write_text(
            'frame=6\nout_time_us=1500000\nprogress=continue\n'
            'frame=7\nout_time_us=2000000\n', encoding='utf-8')
        self.assertAlmostEqual(self.reader().sample(), 0.75)

    def test_missing_or_garbage_files_yield_none(self):
        self.assertIsNone(self.reader().sample())
        self.path.write_text('not a progress stream\n', encoding='utf-8')
        self.assertIsNone(self.reader().sample())
        self.path.write_text('out_time_us=N/A\nframe=N/A\nprogress=continue\n', encoding='utf-8')
        self.assertIsNone(self.reader().sample())

    def test_frame_counter_is_the_fallback_when_out_time_is_absent(self):
        self.path.write_text('frame=12\nprogress=continue\n', encoding='utf-8')
        self.assertAlmostEqual(self.reader(duration=2.0, frames=48).sample(), 0.25)

    def test_fraction_is_clamped(self):
        self.path.write_text('out_time_us=-9223372036854775808\nprogress=continue\n', encoding='utf-8')
        self.assertEqual(self.reader().sample(), 0.0)
        self.path.write_text('out_time_us=99000000\nprogress=continue\n', encoding='utf-8')
        self.assertEqual(self.reader().sample(), 1.0)


class MonitoredNativeTest(_Workspace):
    def test_native_runs_off_the_main_thread_and_frames_increase(self):
        native = StreamingNative(frames=6, sample_delay=0.08)
        progress = RecordingProgress(self.root / PROGRESS_NAME, 6, 'png')
        frames_dir = self.root / 'work' / 'frames'
        frames_dir.mkdir(parents=True)
        config = validate_request(self.request(fps=30, frames=6, duration=0.2,
                                              output=str(self.root / 'out')),
                                  find=lambda name: str(self.exe))
        run_native_export(config, str(frames_dir), native, progress)
        self.assertTrue(native.threads and all(t is not threading.main_thread() for t in native.threads),
                        '监测期间 native 必须在后台线程里跑，主线程才能发布进度')
        render = [item for item in progress.history if item['stage'] == 'render']
        counts = [item['completedFrames'] for item in render]
        self.assertGreaterEqual(len(set(counts)), 2, f'render 阶段应看到多个递增样本：{counts}')
        self.assertEqual(counts, sorted(counts))
        overall = [item['overallFraction'] for item in progress.history]
        self.assertEqual(overall, sorted(overall))
        self.assertTrue(all(item['overallFraction'] < 1.0 for item in progress.history))
        with_fps = [item for item in render if item['fps'] is not None]
        self.assertTrue(with_fps, f'渲染 worker 的 renderSeconds 应能给出 fps：{render}')
        self.assertIsNotNone(with_fps[-1]['estimatedRemainingSeconds'])

    def test_without_a_publisher_the_runner_stays_on_the_main_thread(self):
        native = StreamingNative(frames=2, sample_delay=0.0)
        frames_dir = self.root / 'plain' / 'frames'
        frames_dir.mkdir(parents=True)
        config = validate_request(self.request(fps=30, frames=6, duration=0.2,
                                              output=str(self.root / 'out')),
                                  find=lambda name: str(self.exe))
        run_native_export(config, str(frames_dir), native, None)
        self.assertEqual(native.threads, [threading.current_thread()])

    def test_publish_retries_while_a_reader_holds_the_target_file(self):
        """读者占住目标文件时，假 native 的原子发布必须重试，而不是让测例变红。

        RenderProgressReader 用 ``open(path, 'rb')`` 读文件，Windows 上这个句柄不
        开 FILE_SHARE_DELETE，于是 ``os.replace`` 会以 WinError 32 失败一次或几次。
        生产的 ProgressPublisher 只算这一次没更新、下一次 tick 重试；假 native 没有
        下一次 tick，所以必须在本地做同样的短重试，并且绝不把失败抛给测试。
        """
        path = self.root / 'frames' / PROGRESS_NAME
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('{"version":1,"stage":"prepare"}', encoding='utf-8')
        holder = open(path, 'rb')  # noqa: SIM115 - 故意持有一个不共享删除的读句柄

        def release_later():
            time.sleep(0.03)
            holder.close()

        thread = threading.Thread(target=release_later)
        thread.start()
        try:
            published = publish_worker_progress(path, {'version': 1, 'stage': 'render'})
        finally:
            holder.close()
            thread.join()
        self.assertTrue(published, '读者释放文件后这次发布必须重试成功')
        self.assertEqual(json.loads(path.read_text(encoding='utf-8'))['stage'], 'render')
        self.assertFalse((path.parent / PROGRESS_TMP_NAME).exists(),
                         '重试成功后不允许留下临时进度文件')


class ProgressFlowTest(_Workspace):
    def test_png_never_publishes_an_encode_stage(self):
        config = validate_request(self.request(output=str(self.root / 'out')),
                                  find=lambda name: str(self.exe))
        progress = RecordingProgress(self.root / PROGRESS_NAME, 30, 'png')
        _export_validated(config, FakeNative(frames=30), progress)
        stages = [item['stage'] for item in progress.history]
        self.assertEqual(stages[0], 'prepare')
        self.assertIn('commit', stages)
        self.assertNotIn('encode', stages)
        self.assertEqual(stages[-1], 'done')
        for item in progress.history[:-1]:
            self.assertLess(item['overallFraction'], 1.0)
        self.assertEqual(progress.history[-1]['overallFraction'], 1.0)
        self.assertEqual(progress.history[-1]['completedFrames'], 30)

    def test_mp4_stage_sequence_and_monotonic_fractions(self):
        pipeline = FakePipeline(native=FakeNative(frames=30),
                                ffmpeg=FakeFfmpegProgress(blocks=(0.2, 0.6, 1.0)))
        config = validate_request(self.request(format='mp4', output=str(self.root / 'out.mp4')),
                                  find=lambda name: str(self.exe))
        progress = RecordingProgress(self.root / PROGRESS_NAME, 30, 'mp4')
        _export_validated(config, pipeline, progress)
        stages = [item['stage'] for item in progress.history]
        self.assertEqual(stages[0], 'prepare')
        self.assertIn('render', stages)
        self.assertIn('encode', stages)
        self.assertIn('commit', stages)
        self.assertEqual(stages[-1], 'done')
        fractions = [item['overallFraction'] for item in progress.history]
        self.assertEqual(fractions, sorted(fractions), f'整体比例必须单调：{fractions}')
        self.assertEqual(fractions[-1], 1.0)
        self.assertTrue(all(item['overallFraction'] <= 0.90 + 1e-9
                            for item in progress.history if item['stage'] == 'render'))
        self.assertTrue(all(item['overallFraction'] <= COMMIT_START + 1e-9
                            for item in progress.history if item['stage'] == 'encode'))
        self.assertEqual(progress.history[-1]['completedFrames'], 30)
        self.assertEqual(len(pipeline.ffmpeg.progress_files), 1)

    def test_gif_palettegen_and_encode_are_distinct_phases(self):
        pipeline = FakePipeline(native=FakeNative(frames=30),
                                ffmpeg=FakeFfmpegProgress(blocks=(0.5, 1.0)))
        config = validate_request(self.request(format='gif', output=str(self.root / 'out.gif')),
                                  find=lambda name: str(self.exe))
        progress = RecordingProgress(self.root / PROGRESS_NAME, 30, 'gif')
        _export_validated(config, pipeline, progress)
        phases = [item['phase'] for item in progress.history]
        self.assertIn('palettegen', phases)
        self.assertIn('encode', phases)
        # palettegen 只输出一帧，ffmpeg 报不出它的比例；只有 encode pass 带 -progress。
        self.assertEqual(len(pipeline.ffmpeg.progress_files), 1)
        palette = [item for item in progress.history if item['phase'] == 'palettegen']
        self.assertTrue(palette and all(item['phaseFraction'] is None for item in palette))
        fractions = [item['overallFraction'] for item in progress.history]
        self.assertEqual(fractions, sorted(fractions))
        self.assertTrue(all(item['overallFraction'] < 1.0 for item in progress.history[:-1]))
        self.assertEqual(fractions[-1], 1.0)

    def test_successful_export_leaves_a_done_snapshot_and_no_temp_file(self):
        result = export_animation(self.request(output=str(self.root / 'out')),
                                  runner=FakeNative(frames=30),
                                  progress_path=self.root / PROGRESS_NAME)
        self.assertTrue(result['ok'])
        payload = json.loads((self.root / PROGRESS_NAME).read_text(encoding='utf-8'))
        self.assertEqual(payload['stage'], 'done')
        self.assertEqual(payload['overallFraction'], 1.0)
        self.assertEqual((payload['completedFrames'], payload['totalFrames']), (30, 30))
        self.assertIsInstance(payload['elapsedSeconds'], float)
        self.assertFalse((self.root / PROGRESS_TMP_NAME).exists())

    def test_encode_failure_publishes_a_failed_terminal_state(self):
        pipeline = FakePipeline(native=FakeNative(frames=30), ffmpeg=FakeFfmpeg(code=1, log='boom'))
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(format='mp4', output=str(self.root / 'out.mp4')),
                             runner=pipeline, which=lambda name: str(self.exe),
                             progress_path=self.root / PROGRESS_NAME)
        self.assertEqual(context.exception.code, 'ffmpeg_failed')
        payload = json.loads((self.root / PROGRESS_NAME).read_text(encoding='utf-8'))
        self.assertEqual(payload['stage'], 'failed')
        self.assertEqual(payload['completedFrames'], 30)
        self.assertLess(payload['overallFraction'], 1.0)
        self.assertIn('ffmpeg', payload['message'])

    def test_native_failure_publishes_a_failed_terminal_state(self):
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(output=str(self.root / 'out')),
                             runner=FakeNative(frames=30, ok=False),
                             progress_path=self.root / PROGRESS_NAME)
        self.assertEqual(context.exception.code, 'native_failed')
        payload = json.loads((self.root / PROGRESS_NAME).read_text(encoding='utf-8'))
        self.assertEqual(payload['stage'], 'failed')
        self.assertLess(payload['overallFraction'], 1.0)

    def test_without_a_progress_path_nothing_is_written(self):
        export_animation(self.request(output=str(self.root / 'out')), runner=FakeNative(frames=30))
        self.assertFalse((self.root / PROGRESS_NAME).exists())

    def test_run_job_publishes_into_the_request_directory(self):
        request_path = self.root / 'request.json'
        request_path.write_text(json.dumps(self.request()), encoding='utf-8')
        with mock.patch('export_animation.export_animation', return_value={'ok': True}) as patched:
            _run_job(request_path)
        patched.assert_called_once()
        self.assertEqual(Path(patched.call_args.kwargs['progress_path']),
                         self.root / PROGRESS_NAME)

    def test_run_job_accepts_an_explicit_progress_path(self):
        request_path = self.root / 'request.json'
        request_path.write_text(json.dumps(self.request()), encoding='utf-8')
        explicit = self.root / 'elsewhere' / PROGRESS_NAME
        with mock.patch('export_animation.export_animation', return_value={'ok': True}) as patched:
            _run_job(request_path, explicit)
        self.assertEqual(Path(patched.call_args.kwargs['progress_path']), explicit)


class ManifestProbeNative:
    """假 native：在子进程“运行”期间读取 cleanup.json 与目录内所有权标记。

    真实编辑器（BackgroundJob）只凭这份凭据删除临时目录，所以它必须先把绑定关系
    钉死：目录名 = 前缀 + token、父目录 = 目标父目录、target/jobRoot = 请求与 job
    根目录；而且凭据和目录内所有权标记都必须在任何子进程启动之前就存在。
    """

    def __init__(self, root, frames=30, width=64, height=32):
        self.root = Path(root)
        self.inner = FakeNative(frames=frames, width=width, height=height)
        self.record = None
        self.marker = None
        self.marker_is_file = False
        self.temp_existed = False
        self.frames_existed = False
        self.manifest_existed = False

    def __call__(self, argv, timeout, *, timeout_code='process_timeout', cwd=None):
        self.manifest_existed = (self.root / CLEANUP_NAME).is_file()
        self.record = json.loads((self.root / CLEANUP_NAME).read_text(encoding='utf-8'))
        temp = Path(self.record['tempDirectory'])
        self.temp_existed = temp.is_dir()
        self.frames_existed = (temp / 'frames').is_dir()
        marker_path = temp / OWNERSHIP_NAME
        self.marker_is_file = marker_path.is_file() and not marker_path.is_symlink()
        if self.marker_is_file:
            self.marker = json.loads(marker_path.read_text(encoding='utf-8'))
        return self.inner(argv, timeout, timeout_code=timeout_code, cwd=cwd)


class CleanupRecordTest(_Workspace):
    """cleanup.json：父进程清理临时目录的唯一凭据。"""

    def record_path(self):
        return self.root / CLEANUP_NAME

    def test_success_removes_the_temp_dir_and_the_record(self):
        result = export_animation(self.request(output=str(self.root / 'out')),
                                  runner=FakeNative(frames=30),
                                  progress_path=self.root / PROGRESS_NAME)
        self.assertTrue(result['ok'])
        self.assertTrue((self.root / 'out').is_dir())
        self.assertEqual(self.temp_dirs(), [])
        self.assertFalse(self.record_path().exists(), '临时目录已删，凭据也必须丢掉')
        self.assertFalse((self.root / CLEANUP_TMP_NAME).exists())

    def test_failure_removes_the_temp_dir_and_the_record(self):
        with self.assertRaises(ExportError):
            export_animation(self.request(format='mp4', output=str(self.root / 'out.mp4')),
                             runner=FakePipeline(native=FakeNative(frames=30),
                                                 ffmpeg=FakeFfmpeg(code=1, log='boom')),
                             which=lambda name: str(self.exe),
                             progress_path=self.root / PROGRESS_NAME)
        self.assertFalse((self.root / 'out.mp4').exists())
        self.assertEqual(self.temp_dirs(), [])
        self.assertFalse(self.record_path().exists())

    def test_record_is_published_before_any_child_runs_and_binds_everything(self):
        probe = ManifestProbeNative(self.root, frames=30)
        export_animation(self.request(output=str(self.root / 'out')), runner=probe,
                         progress_path=self.root / PROGRESS_NAME)
        self.assertTrue(probe.manifest_existed, '凭据必须在任何子进程启动之前就存在')
        self.assertTrue(probe.temp_existed, '凭据指向的临时目录必须已经存在')
        self.assertTrue(probe.frames_existed, '凭据指向的就是这次导出的临时目录')

        record = probe.record
        self.assertEqual(record['version'], CLEANUP_VERSION)
        self.assertEqual(record['marker'], CLEANUP_MARKER)
        token = record['token']
        self.assertRegex(token, CLEANUP_TOKEN_RE, 'token 必须是 32 位小写十六进制')
        for key in ('jobRoot', 'outputParent', 'tempDirectory', 'target'):
            with self.subTest(key=key):
                self.assertTrue(os.path.isabs(record[key]), f'{key} 必须是绝对路径：{record[key]}')
        temp = Path(record['tempDirectory'])
        self.assertEqual(temp.name, TEMP_PREFIX + token)
        self.assertEqual(Path(record['outputParent']), self.root)
        self.assertEqual(temp.parent, self.root)
        self.assertEqual(record['target'], str(self.root / 'out'))
        self.assertEqual(Path(record['jobRoot']), self.root)
        self.assertNotEqual(Path(record['target']), temp)

        # 目录内的所有权标记：只有本任务创建的目录才有它，编辑器删除前要核对。
        self.assertTrue(probe.marker_is_file, '临时目录里必须有一个普通文件的 ownership.json')
        marker = probe.marker
        self.assertEqual(marker['version'], CLEANUP_VERSION)
        self.assertEqual(marker['marker'], OWNERSHIP_MARKER)
        self.assertEqual(marker['token'], token)
        self.assertEqual(marker['jobRoot'], record['jobRoot'])
        self.assertEqual(marker['target'], record['target'])

    def test_the_record_is_published_only_after_mkdir_and_the_marker(self):
        # 发布凭据的那一刻，目录必须已经存在，且标记已经写在里面：否则被强杀的
        # 窗口里就会出现“凭据指向一个不是自己创建的目录”。
        seen = []

        def probe_record(path, record):
            temp = Path(record['tempDirectory'])
            seen.append((temp.is_dir(), (temp / OWNERSHIP_NAME).is_file()))
            return _write_cleanup_record(path, record)

        with mock.patch('export_animation._write_cleanup_record', side_effect=probe_record):
            export_animation(self.request(output=str(self.root / 'out')),
                             runner=FakeNative(frames=30),
                             progress_path=self.root / PROGRESS_NAME)
        self.assertEqual(seen, [(True, True)], '凭据只能在目录与标记都就绪之后发布')

    def test_a_kill_before_the_record_is_published_leaves_no_record(self):
        # 模拟“mkdir 成功、凭据还没发布就被强杀”：盘上不许出现任何凭据，编辑器
        # 因此绝不会去动那个目录。
        with mock.patch('export_animation._write_cleanup_record',
                        side_effect=OSError('killed before the record was published')):
            with self.assertRaises(OSError):
                _make_recorded_temp_dir(str(self.root), self.record_path(), str(self.root / 'out'))
        self.assertFalse(self.record_path().exists(), '没有发布成功的凭据就不该留在盘上')
        self.assertFalse((self.root / CLEANUP_TMP_NAME).exists())
        self.assertEqual(self.temp_dirs(), [], '未登记的残留目录应尽量自己删掉')

    def test_the_ownership_marker_is_written_exclusively(self):
        parent = self.root / 'exclusive'
        parent.mkdir()
        created = Path(_make_temp_dir(TEMP_PREFIX, str(parent)))
        _write_ownership_marker(str(created), 'c' * 32, str(self.root), str(self.root / 'out'))
        with self.assertRaises(FileExistsError):
            _write_ownership_marker(str(created), 'd' * 32, str(self.root), str(self.root / 'out'))
        marker = json.loads((created / OWNERSHIP_NAME).read_text(encoding='utf-8'))
        self.assertEqual(marker['token'], 'c' * 32)

    def test_record_is_next_to_progress_json_and_needs_a_binding_target(self):
        probe = ManifestProbeNative(self.root / 'job')
        (self.root / 'job').mkdir()
        export_animation(self.request(output=str(self.root / 'out')), runner=probe,
                         progress_path=self.root / 'job' / PROGRESS_NAME)
        self.assertEqual(Path(probe.record['jobRoot']), self.root / 'job')

    def test_without_a_progress_path_nothing_is_recorded(self):
        export_animation(self.request(output=str(self.root / 'out')), runner=FakeNative(frames=30))
        self.assertFalse(self.record_path().exists())
        self.assertFalse((self.root / PROGRESS_NAME).exists())

    def test_a_record_survives_when_the_temp_dir_could_not_be_removed(self):
        # 临时目录删不掉（例如还有子进程占着文件）时，凭据必须留给编辑器侧的
        # BackgroundJob；丢掉凭据就等于放弃清理。
        with mock.patch('export_animation.shutil.rmtree'):
            result = export_animation(self.request(output=str(self.root / 'out')),
                                      runner=FakeNative(frames=30),
                                      progress_path=self.root / PROGRESS_NAME)
        self.assertTrue(result['ok'])
        leftovers = self.temp_dirs()
        self.assertEqual(len(leftovers), 1, '被 mock 掉的 rmtree 应该留下临时目录')
        record = json.loads(self.record_path().read_text(encoding='utf-8'))
        self.assertEqual(Path(record['tempDirectory']), leftovers[0])
        self.assertEqual(Path(record['target']), self.root / 'out')

    def test_manifest_write_failure_is_a_temp_failure(self):
        # 凭据写不下就不能建临时目录：否则被强杀时就再没人知道该删哪个目录。
        blocked = self.root / 'blocked'
        blocked.write_text('not a directory', encoding='utf-8')
        with self.assertRaises(ExportError) as context:
            export_animation(self.request(output=str(self.root / 'out')),
                             runner=FakeNative(frames=30),
                             progress_path=blocked / PROGRESS_NAME)
        self.assertEqual(context.exception.code, 'temp_failed')
        self.assertEqual(self.temp_dirs(), [], '写不下凭据就不许留下临时目录')

    def test_a_colliding_directory_is_never_recorded(self):
        # 撞名目录不是本任务创建的：既不能记进凭据，也不能被写进所有权标记，更不能
        # 被动到。撞名时先独占 mkdir 失败，才会换 token，所以那个目录从没被登记。
        parent = self.root / 'collide'
        parent.mkdir()
        intruder = parent / (TEMP_PREFIX + 'a' * 32)
        intruder.mkdir()
        (intruder / 'keep.txt').write_text('not ours', encoding='utf-8')

        record_path = self.root / CLEANUP_NAME
        with mock.patch('export_animation.uuid.uuid4',
                        side_effect=[type('U', (), {'hex': 'a' * 32})(),
                                     type('U', (), {'hex': 'b' * 32})()]):
            created = _make_recorded_temp_dir(str(parent), record_path, str(self.root / 'out'))
        self.assertEqual(Path(created), parent / (TEMP_PREFIX + 'b' * 32))
        self.assertEqual((intruder / 'keep.txt').read_text(encoding='utf-8'), 'not ours')
        self.assertFalse((intruder / OWNERSHIP_NAME).exists(), '撞名目录里不许出现所有权标记')
        record = json.loads(record_path.read_text(encoding='utf-8'))
        self.assertEqual(record['token'], 'b' * 32)
        self.assertEqual(Path(record['tempDirectory']), Path(created))
        self.assertFalse((parent / CLEANUP_TMP_NAME).exists())
        marker = json.loads((Path(created) / OWNERSHIP_NAME).read_text(encoding='utf-8'))
        self.assertEqual(marker['token'], 'b' * 32)
        self.assertEqual(Path(marker['target']), self.root / 'out')
        self.assertEqual(Path(marker['jobRoot']), self.root)


if __name__ == '__main__':
    unittest.main()
