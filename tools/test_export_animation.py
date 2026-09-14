"""export_animation 的单元测试。

全部用注入的假 runner（原生 editor / ffmpeg）与真实临时文件系统，不启动
pdflatex、ffmpeg 或原生程序。运行方式::

    python -m unittest tools.test_export_animation
    # 或在该目录下: python -m unittest test_export_animation
"""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import unittest
import zlib
from unittest import mock

from export_animation import (ExportError, MAX_FRAMES, build_ffmpeg_gif,
                              build_ffmpeg_mp4, commit_file, export_animation, main,
                              validate_request, validate_target, _make_temp_dir,
                              _run_hidden)


def _png_bytes(width, height):
    """只需要前 24 字节合法（IHDR），verify_frames 不解码像素。"""
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IEND', b'')


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
                         min(3600.0, 180.0 + 2.0 * 30))
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


if __name__ == '__main__':
    unittest.main()
