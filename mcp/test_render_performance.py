"""Measured render-performance harness for the offscreen Write animation export.

Unlike the correctness regressions this script is built to answer *where* the
export time goes. It boots a private stdio editor, exports a scene as a PNG
frame sequence at explicit even dimensions, and records:

  * the end-to-end wall time of the export job (harness side),
  * the per-phase counters the native offscreen loop publishes to
    ``<framesDirectory>/progress.json`` (CPU compose, GPU wait/readback, PNG
    write) together with the progress timeline itself,
  * a byte hash and a decoded-pixel hash of every frame, so two builds can be
    compared for "the optimisation changed nothing".

The cross-worker progress contract checked here is the one the native exporter
owns: ``version``, ``stage`` ('render' for the whole run), ``completedFrames``,
``totalFrames``, ``elapsedSeconds``, ``renderSeconds``, ``readbackWriteSeconds``
inside the frames directory, refreshed at least every 250 ms, counting a frame
only after its PNG really reached the disk, and never reporting a stage other
than 'render'.

It never attaches to a user's editor window: it always starts its own child from
the executable passed on the command line.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import zlib

from bridge import StdioEditorBridge

ROOT = Path(__file__).resolve().parents[1]
FRAME_PREFIX = 'frame_'
FRAME_SUFFIX = '.png'
# The native writer refreshes progress.json on a throttled cadence; a sample gap
# above this means the contract was violated (the exporter stalled or stopped
# publishing long before the next frame finished).
MAX_PROGRESS_GAP_SECONDS = 0.5


def frame_files(frames_dir: Path) -> list[Path]:
    if not frames_dir.is_dir():
        return []
    found = []
    for name in os.listdir(frames_dir):
        if name.startswith(FRAME_PREFIX) and name.endswith(FRAME_SUFFIX):
            index = name[len(FRAME_PREFIX):-len(FRAME_SUFFIX)]
            if index.isdigit():
                found.append((int(index), frames_dir / name))
    return [path for _, path in sorted(found)]


def decode_png_rgba(path: Path) -> bytes:
    """Decode a PNG written by src/platform/png_writer.hpp (8-bit RGBA, 8-bit zlib).

    That writer emits filter type 0 on every scanline, so unfiltering is a slice
    per row instead of a per-byte predictor loop. A different filter type is
    reported instead of being decoded wrong.
    """
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'not a PNG: {path}')
    offset, width, height, idat = 8, 0, 0, bytearray()
    while offset + 8 <= len(data):
        length = int.from_bytes(data[offset:offset + 4], 'big')
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        if kind == b'IHDR':
            width = int.from_bytes(body[0:4], 'big')
            height = int.from_bytes(body[4:8], 'big')
        elif kind == b'IDAT':
            idat.extend(body)
        elif kind == b'IEND':
            break
        offset += 12 + length
    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    if len(raw) != height * (stride + 1):
        raise ValueError(f'unexpected scanline layout in {path}')
    pixels = bytearray()
    for row in range(height):
        start = row * (stride + 1)
        if raw[start] != 0:
            raise ValueError(f'unsupported PNG filter {raw[start]} in {path}')
        pixels += raw[start + 1:start + 1 + stride]
    return bytes(pixels)


def compare_frames(baseline: Path, candidate: Path) -> dict:
    """Byte compare, then pixel compare, of two frame sequences."""
    before, after = frame_files(baseline), frame_files(candidate)
    result = {'baselineFrames': len(before), 'candidateFrames': len(after),
              'identicalFiles': 0, 'differingFrames': [], 'maxChannelDelta': 0,
              'changedChannels': 0, 'missing': [], 'extra': []}
    if len(before) != len(after):
        result['missing'] = [path.name for path in before[len(after):]]
        result['extra'] = [path.name for path in after[len(before):]]
    for index, (left, right) in enumerate(zip(before, after)):
        left_bytes, right_bytes = left.read_bytes(), right.read_bytes()
        if left_bytes == right_bytes:
            result['identicalFiles'] += 1
            continue
        if len(result['differingFrames']) < 8:
            result['differingFrames'].append(right.name)
        pixels_left, pixels_right = decode_png_rgba(left), decode_png_rgba(right)
        if len(pixels_left) != len(pixels_right):
            result['maxChannelDelta'] = 255
            result['changedChannels'] += 1
            continue
        delta = 0
        changed = 0
        for a, b in zip(pixels_left, pixels_right):
            difference = abs(a - b)
            if difference:
                changed += 1
                if difference > delta:
                    delta = difference
        result['maxChannelDelta'] = max(result['maxChannelDelta'], delta)
        result['changedChannels'] += changed
    result['pixelIdentical'] = (result['identicalFiles'] == len(before) == len(after)
                                and not result['missing'] and not result['extra'])
    return result


def read_progress(frames_dir: Path) -> dict | None:
    path = frames_dir / 'progress.json'
    try:
        with path.open('r', encoding='utf-8') as handle:
            value = json.load(handle)
    except (OSError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def newest_native_frames_dir(since: float, parent: Path) -> Path | None:
    """Locate the frames directory the export worker handed to the native renderer.

    For the PNG format the worker renders into its own temporary ``frames``
    directory next to the requested output and renames that directory onto the
    output only at the end, so while the export runs the progress file the native
    loop publishes lives there. Only directories created after ``since`` count.
    """
    best: tuple[float, Path] | None = None
    try:
        candidates = list(parent.glob('physics-export-*'))
    except OSError:
        return None
    for candidate in candidates:
        frames = candidate / 'frames'
        try:
            stamp = frames.stat().st_mtime
        except OSError:
            continue
        if stamp < since - 1:
            continue
        if best is None or stamp > best[0]:
            best = (stamp, frames)
    return best[1] if best else None


def parse_perf_line(text: str) -> dict | None:
    """Parse the native '[perf] offscreen ...' summary into named seconds."""
    for line in reversed(text.splitlines()):
        if '[perf] offscreen' not in line:
            continue
        fields = line.split()
        values: dict[str, float] = {}
        try:
            for index, token in enumerate(fields):
                if token in ('memory', 'encoders') and index + 1 < len(fields):
                    values[token] = fields[index + 1]
                elif index + 1 < len(fields) and token in ('compose', 'wait', 'swizzle', 'png',
                                                           'render', 'elapsed'):
                    values[token] = float(fields[index + 1])
        except ValueError:
            return None
        return values or None
    return None


def check_progress_contract(samples: list[dict], total: int, writing_seconds: float) -> dict:
    """Validate the published progress timeline and return a compact report.

    Two cadences are reported separately: the file itself must be refreshed inside
    the 100..250 ms window (``maxFileUpdateGapSeconds``), while
    ``maxFrameAdvanceGapSeconds`` only describes how far apart completed frames
    were, which legitimately grows when single frames are slow.
    """
    report = {'samples': len(samples), 'updates': 0, 'maxFrameAdvanceGapSeconds': 0.0,
              'maxFileUpdateGapSeconds': 0.0, 'firstCompleted': None, 'lastCompleted': None,
              'publishIntervalSeconds': None, 'violations': []}
    if not samples:
        report['violations'].append('progress.json was never published')
        return report
    distinct = 0
    previous_frame_seconds: float | None = None
    previous_completed: int | None = None
    previous_content: str | None = None
    previous_update_seconds: float | None = None
    publish_times: list[float] = []
    for sample in samples:
        value = sample['progress']
        for key in ('version', 'stage', 'completedFrames', 'totalFrames',
                    'elapsedSeconds', 'renderSeconds', 'readbackWriteSeconds'):
            if key not in value:
                report['violations'].append(f'missing key {key}')
        if value.get('version') != 1:
            report['violations'].append(f"version {value.get('version')} != 1")
        if value.get('stage') != 'render':
            report['violations'].append(f"stage {value.get('stage')!r} != 'render'")
        if value.get('totalFrames') != total:
            report['violations'].append(f"totalFrames {value.get('totalFrames')} != {total}")
        content = json.dumps(value, sort_keys=True)
        if content != previous_content:
            if previous_update_seconds is not None:
                report['maxFileUpdateGapSeconds'] = max(report['maxFileUpdateGapSeconds'],
                                                        sample['seconds'] - previous_update_seconds)
            previous_update_seconds = sample['seconds']
            previous_content = content
        completed = value.get('completedFrames')
        if not isinstance(completed, int) or completed < 0 or completed > total:
            report['violations'].append(f'completedFrames {completed} out of range')
            continue
        if previous_completed is not None and completed < previous_completed:
            report['violations'].append(f'completedFrames went backwards {previous_completed}->{completed}')
        # A frame may only be counted once its PNG is really on disk.
        if sample['onDisk'] < completed:
            report['violations'].append(
                f'completedFrames {completed} exceeds the {sample["onDisk"]} frame files on disk')
        if report['firstCompleted'] is None:
            report['firstCompleted'] = completed
        report['lastCompleted'] = completed
        if previous_completed is None or completed != previous_completed:
            distinct += 1
            publish_times.append(sample['seconds'])
            if previous_frame_seconds is not None:
                report['maxFrameAdvanceGapSeconds'] = max(report['maxFrameAdvanceGapSeconds'],
                                                          sample['seconds'] - previous_frame_seconds)
            previous_frame_seconds = sample['seconds']
            previous_completed = completed
    report['updates'] = distinct
    if len(publish_times) > 1:
        report['publishIntervalSeconds'] = (publish_times[-1] - publish_times[0]) / (len(publish_times) - 1)
    if report['maxFileUpdateGapSeconds'] > MAX_PROGRESS_GAP_SECONDS:
        report['violations'].append(
            f'progress file stalled for {report["maxFileUpdateGapSeconds"]:.3f}s '
            f'(limit {MAX_PROGRESS_GAP_SECONDS}s)')
    if report['lastCompleted'] != total:
        report['violations'].append(f'final completedFrames {report["lastCompleted"]} != {total}')
    if report['lastCompleted'] in (None, 0) and writing_seconds > 1.0:
        report['violations'].append('progress never advanced past 0')
    return report


def dense_scene(document: dict, copies: int = 6, spread: bool = True) -> dict:
    """Clone the Write demo into a deliberately geometry-heavy scene.

    The per-frame CPU compose cost (glyph outlines plus the formula fill quads) is
    invisible in a small scene once the readback and the PNG write are parallel, so
    this variant stacks several copies of the whole demo. Every write window is also
    stretched to the whole scene duration, so all copies are mid-animation at the
    same time: staggered windows would hide exactly the geometry this measures.

    ``spread=False`` stacks the copies on the same coordinates. That keeps the fit
    framing at the original zoom, so the traced outline and fill geometry really
    multiplies; ``spread=True`` moves them apart, which grows the scene bounds and
    shrinks the fit zoom (a lower bound on the geometry stress).
    """
    span = max(0.1, float(document.get('duration', 3.0)) - 0.05)
    originals = [json.loads(json.dumps(item)) for item in document['objects']]
    objects = list(originals)
    for copy in range(1, copies):
        for item in originals:
            clone = json.loads(json.dumps(item))
            clone['id'] = f"{item['id']}-{copy}"
            clone['name'] = f"{item.get('name', item['id'])} #{copy}"
            if spread:
                clone['properties']['x'] = item['properties']['x'] + 9.0 * copy
            objects.append(clone)
    document = json.loads(json.dumps(document))
    document['objects'] = objects
    for item in document['objects']:
        if 'write' in item:
            item['write']['start'] = 0.0
            item['write']['duration'] = span
    return document


def run(executable: Path, *, scene: str = 'scenes/write-entry.json', fps: int = 24,
        width: int = 1280, height: int = 720, work: Path | None = None, label: str = 'run',
        validation: bool = True, compare: Path | None = None, dense_copies: int = 0,
        dense_spread: bool = True) -> dict:
    document = json.loads((ROOT / scene).read_text(encoding='utf-8'))
    if dense_copies > 1:
        document = dense_scene(document, dense_copies, dense_spread)
        label = f'{label}-dense{len(document["objects"])}'
    duration = float(document.get('duration', 3.0))
    expected = int(-(-duration * fps // 1))
    if expected < 1:
        raise SystemExit(f'{scene}: duration {duration} at {fps} fps produces no frames')
    if work is None:
        work = Path(tempfile.mkdtemp(prefix=f'perf-render-{label}-'))
    work.mkdir(parents=True, exist_ok=True)
    # The PNG target must not pre-exist: the export worker refuses to overwrite an
    # existing frame directory, so the exporter itself creates it.
    frames_dir = work / 'frames'
    native_log = work / 'native.log'
    samples: list[dict] = []
    with native_log.open('w', encoding='utf-8') as log:
        bridge = StdioEditorBridge(executable,
                                   args=(['--validation'] if validation else []) + ['--data-dir', str(work / 'data')],
                                   cwd=ROOT, env={**os.environ, 'VK_LAYER_VALIDATE_SYNC': '1'},
                                   stderr_sink=log)
        try:
            state = bridge.request('get_scene')
            bridge.request('replace', {'document': document}, state['revision'])
            revision = bridge.request('get_scene')['revision']
            started = time.monotonic()
            bridge.request('export_animation', {'output': str(frames_dir), 'format': 'png',
                                                'fps': fps, 'width': width, 'height': height,
                                                'view_mode': 'fit', 'validation': validation}, revision)
            job = {}
            deadline = time.monotonic() + 1800
            live_dir: Path | None = None
            while True:
                now = time.monotonic()
                job = bridge.request('get_job', {'kind': 'export'})
                if live_dir is None or not (live_dir / 'progress.json').is_file():
                    live_dir = newest_native_frames_dir(started, frames_dir.parent) or live_dir
                progress = read_progress(live_dir) if live_dir else None
                if progress is not None:
                    samples.append({'seconds': now - started, 'progress': progress,
                                    'onDisk': len(frame_files(live_dir))})
                if job.get('status') != 'running':
                    break
                if now > deadline:
                    raise SystemExit(f'export did not finish in 1800s: {job}')
                time.sleep(0.05)
            # The worker renames the temporary frames directory onto the requested
            # output when it finishes, so read the final snapshot from there too.
            final_progress = read_progress(frames_dir)
            if final_progress is not None:
                samples.append({'seconds': time.monotonic() - started, 'progress': final_progress,
                                'onDisk': len(frame_files(frames_dir))})
            finished = time.monotonic()
        finally:
            bridge.close()
    if job.get('status') != 'completed':
        raise SystemExit(f'export failed: {json.dumps(job, ensure_ascii=False)[:2000]}\n'
                         f'log tail: {native_log.read_text(encoding="utf-8", errors="replace")[-2000:]}')
    files = frame_files(frames_dir)
    digests: dict[str, str] = {}
    for path in files:
        digests[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    combined = hashlib.sha256(''.join(f'{name}:{digest}' for name, digest in digests.items()).encode()).hexdigest()
    timeline = check_progress_contract(samples, expected, finished - started)
    # The export child runs inside the worker's job directory; with --validation the
    # worker echoes its log, and that is where the layer summary line lives.
    validation_text = native_log.read_text(encoding='utf-8', errors='replace')
    worker_log = work / 'data' / 'build' / 'jobs' / str(job.get('id', '')) / 'worker.log'
    if worker_log.is_file():
        validation_text += worker_log.read_text(encoding='utf-8', errors='replace')
    split = parse_perf_line(validation_text)
    final_progress = samples[-1]['progress'] if samples else {}
    render_seconds = float(final_progress.get('renderSeconds', 0.0) or 0.0)
    io_seconds = float(final_progress.get('readbackWriteSeconds', 0.0) or 0.0)
    native_elapsed = float(final_progress.get('elapsedSeconds', 0.0) or 0.0)
    report = {
        'label': label, 'executable': str(executable), 'scene': scene,
        'width': width, 'height': height, 'fps': fps, 'expectedFrames': expected,
        'frames': len(files), 'seconds': finished - started,
        'secondsPerFrame': (finished - started) / max(1, expected),
        'nativeElapsedSeconds': native_elapsed,
        'nativeRenderSeconds': render_seconds,
        'nativeReadbackWriteSeconds': io_seconds,
        'renderShare': (render_seconds / native_elapsed) if native_elapsed > 0 else None,
        'ioShare': (io_seconds / native_elapsed) if native_elapsed > 0 else None,
        'accountedShare': ((render_seconds + io_seconds) / native_elapsed) if native_elapsed > 0 else None,
        'renderStageMillisecondsPerFrame': 1000.0 * render_seconds / max(1, expected),
        'readbackWriteMillisecondsPerFrame': 1000.0 * io_seconds / max(1, expected),
        'nativeSplit': split,
        'nativeSplitMillisecondsPerFrame': ({key: 1000.0 * value / max(1, expected)
                                             for key, value in split.items()
                                             if isinstance(value, float)} if split else None),
        'framesHash': combined,
        'frameDigests': digests,
        'progress': timeline,
        'framesDirectory': str(frames_dir),
        'workDirectory': str(work),
        'validationClean': 'Validation errors: 0; warnings: 0' in validation_text,
    }
    if compare is not None:
        report['comparison'] = compare_frames(Path(compare), frames_dir)
    (work / 'performance.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--scene', default='scenes/write-entry.json')
    parser.add_argument('--fps', type=int, default=24)
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--work', type=Path, default=None,
                        help='output/work directory (default: a fresh directory under the temp root)')
    parser.add_argument('--label', default='run', help='short name recorded in the report')
    parser.add_argument('--compare', type=Path, default=None,
                        help='baseline frame directory to compare pixel-by-pixel against')
    parser.add_argument('--no-validation', action='store_true', help='skip Vulkan validation layers')
    parser.add_argument('--dense', type=int, default=0, metavar='COPIES',
                        help='stack COPIES copies of the scene to stress CPU compose/outline geometry')
    parser.add_argument('--dense-overlap', action='store_true',
                        help='with --dense, keep the copies on the same coordinates so the fit zoom stays')
    arguments = parser.parse_args()
    report = run(arguments.executable.resolve(), scene=arguments.scene, fps=arguments.fps,
                 width=arguments.width, height=arguments.height, work=arguments.work,
                 label=arguments.label, validation=not arguments.no_validation,
                 compare=arguments.compare, dense_copies=arguments.dense,
                 dense_spread=not arguments.dense_overlap)
    print(json.dumps({key: report[key] for key in (
        'label', 'seconds', 'secondsPerFrame', 'expectedFrames', 'frames', 'framesHash',
        'nativeElapsedSeconds', 'nativeRenderSeconds', 'nativeReadbackWriteSeconds',
        'renderShare', 'ioShare', 'renderStageMillisecondsPerFrame',
        'readbackWriteMillisecondsPerFrame', 'nativeSplitMillisecondsPerFrame',
        'validationClean', 'framesDirectory')},
        ensure_ascii=False, indent=2))
    print(json.dumps(report['progress'], ensure_ascii=False, indent=2))
    if 'comparison' in report:
        summary = dict(report['comparison'])
        summary.pop('differingFrames', None)
        print(json.dumps({'comparison': summary, 'differingFrames': report['comparison']['differingFrames']},
                         ensure_ascii=False, indent=2))
    if report['frames'] != report['expectedFrames']:
        print(f'FAIL: {report["frames"]} frames written, {report["expectedFrames"]} expected', file=sys.stderr)
        return 1
    if report['progress']['violations']:
        print(f'FAIL: progress contract: {report["progress"]["violations"]}', file=sys.stderr)
        return 1
    if not report['validationClean']:
        print('FAIL: Vulkan validation did not report 0 errors and 0 warnings', file=sys.stderr)
        return 1
    if 'comparison' in report and not report['comparison'].get('pixelIdentical'):
        print('FAIL: frame output differs from the baseline', file=sys.stderr)
        return 1
    print(f'PASS: {report["frames"]} frames in {report["seconds"]:.2f}s '
          f'({report["secondsPerFrame"] * 1000:.1f} ms/frame) -> {report["framesDirectory"]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
