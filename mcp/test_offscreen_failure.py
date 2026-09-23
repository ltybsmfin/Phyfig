"""Real native regression for the offscreen exporter's *failure* path.

The editor's PNG sequence export encodes frames in a background pool
(``src/platform/offscreen_export.cpp``). When one encoder cannot write its frame,
the export must fail visibly and promptly:

* the process exits non-zero and never writes a ``result.json`` that claims
  ``ok:true``;
* it exits inside a bounded wall clock (a dead encoder pool must never hang the
  render thread);
* the unwind leaves no Vulkan resource errors behind (``--validation``).

Two defects in that file are guarded here:

* ``OffscreenExporter::waitForPendingWrites()`` used to return as soon as every
  staging slot was back and only then look at ``encodeFailure_``: an export whose
  encoder had already failed was announced as a success. A one-frame export whose
  only frame cannot be written is the smallest deterministic window for that bug
  (the failed encoder returns its slot, so "all slots free" is true immediately);
* the encoder released its staging slot before it published the completion
  counter, so a waiter could observe a drained queue while the published counter
  still lagged. A successful short sequence must therefore end with
  ``completedFrames == totalFrames ==`` the number of PNG files really on disk.

How the failure is triggered deterministically: ``frame_NNNNNN.png`` is
pre-created as a *directory* inside this run's own frames directory, so opening it
as a file (``image::png::write``) fails. Nothing outside the test's private work
directory is touched, no user editor/window/pipe is used, and the only processes
started are this test's own editor and ``--export-job`` children.

The snapshot is obtained from a real private stdio editor session
(``export_animation`` writes ``snapshot.json`` through the production
``Editor::setupExport`` contract), then reused verbatim for the direct
``--export-job`` runs, so a hand-built snapshot can never drift from that contract.

Usage::

    python mcp/test_offscreen_failure.py <physics_editor.exe> [work-dir]

``work-dir`` defaults to ``build/verification``; the test always creates a unique
``offscreen-failure-<uuid>`` directory inside it and leaves a ``report.json``
there. Suggested CTest registration (this file only adds itself; it does not touch
CMakeLists.txt)::

    add_test(NAME editor_offscreen_failure
             COMMAND "${PHYSICS_MCP_PYTHON}" "${PROJECT_SOURCE_DIR}/mcp/test_offscreen_failure.py"
                     "$<TARGET_FILE:physics_editor>")
    set_tests_properties(editor_offscreen_failure PROPERTIES
        TIMEOUT 600
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1")

600 s is deliberately generous: a healthy run spends a few seconds on the baseline
snapshot, about a second per one/four-frame failure case and a few seconds per
success repeat, while every child process here is bounded at 180 s.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time
import uuid

from bridge import StdioEditorBridge

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_WORK_PARENT = ROOT / 'build' / 'verification'

# The snapshot carries this exact rate (``Editor::setupExport`` requires it), so
# every direct request below reuses it instead of inventing a second time base.
FPS = 8
DURATION = 2.0
BASELINE_SIZE = (320, 180)
FAILURE_SIZE = (64, 48)
SUCCESS_SIZE = (240, 160)
SUCCESS_FRAMES = 12
SUCCESS_REPEATS = 4

FRAME_PATTERN = 'frame_%06d.png'
PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'

# The failing encoder's own text. Asserting on it proves a non-zero exit came from
# the blocked PNG file and not from an unrelated startup or Vulkan failure.
PNG_FAILURE_MARKERS = ('Cannot open PNG output', 'Offscreen PNG encoding failed')
VALIDATION_ERROR_MARKER = 'Validation Error'
VALIDATION_SUMMARY = re.compile(r'Validation errors:\s*(\d+);\s*warnings:\s*(\d+)')

# Per-child wall-clock bounds. The failure cases exist to prove the exporter does
# not hang once its encoder pool is dead, so a timeout is a real failure here.
BRIDGE_JOB_TIMEOUT = 180.0
FAILURE_TIMEOUT = 180.0
SUCCESS_TIMEOUT = 180.0


def child_env():
    return {**os.environ, 'VK_LAYER_VALIDATE_SYNC': '1'}


def frame_files(directory):
    """Regular frame files only.

    The failure trigger is a *directory* named like a frame, so counting raw
    glob matches would count the trigger itself as an exported frame.
    """
    if not directory.is_dir():
        return []
    return sorted(item for item in directory.glob('frame_*.png') if item.is_file())


def read_json(path):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None


def write_json(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding='utf-8')


def validation_errors(text):
    """Error-severity validation lines (the layer prefixes them with 'Validation Error')."""
    return [line for line in text.splitlines() if VALIDATION_ERROR_MARKER in line]


def validation_summary(text):
    match = VALIDATION_SUMMARY.search(text)
    return None if match is None else (int(match.group(1)), int(match.group(2)))


def assert_clean_validation(log, where, require_summary):
    errors = validation_errors(log)
    assert not errors, f'Vulkan validation reported errors ({where}): {errors[:4]}'
    summary = validation_summary(log)
    if require_summary:
        assert summary is not None, f'the exporter never printed its validation summary ({where})'
        assert summary[0] == 0, f'the exporter counted {summary[0]} validation errors ({where})'
    return summary


def write_request(run_dir, snapshot, frames, size):
    """The exact subset ``apps/editor/main.cpp --export-job`` reads.

    ``frames`` is the request's loop count (the exporter validates 1..3600); it is
    independent of ``ceil(duration*fps)`` because this test drives the native CLI
    directly, exactly like ``tools/export_animation.py`` does with its own copy.
    """
    frames_dir = run_dir / 'frames'
    request = {
        'snapshot': str(snapshot),
        'framesDirectory': str(frames_dir),
        'width': size[0],
        'height': size[1],
        'fps': FPS,
        'frames': frames,
        'validation': True,
    }
    request_path = run_dir / 'request.json'
    write_json(request_path, request)
    return request_path, frames_dir


def acquire_snapshot(executable, work):
    """Let the product write one real snapshot, then copy it into the test's work dir.

    A private stdio editor (``--stdio --data-dir <work>/baseline/data``) runs a tiny
    16-frame PNG export; ``Editor::jobRequest`` writes ``snapshot.json`` into that
    job's folder before the worker starts, so the file is a genuine product artifact
    with real formula resources and a real ``fit`` viewport.
    """
    folder = work / 'baseline'
    data = folder / 'data'
    data.mkdir(parents=True, exist_ok=True)
    log_path = folder / 'editor.log'
    with log_path.open('w', encoding='utf-8') as log:
        bridge = StdioEditorBridge(executable, args=['--validation', '--data-dir', str(data)],
                                   cwd=ROOT, env=child_env(), stderr_sink=log)
        try:
            state = bridge.request('get_scene')
            bridge.request('apply', {'commands': [{'op': 'settings', 'duration': DURATION}]},
                           state['revision'])
            revision = bridge.request('get_scene')['revision']
            started = bridge.request('export_animation', {
                'output': str(folder / 'baseline-frames'), 'format': 'png',
                'width': BASELINE_SIZE[0], 'height': BASELINE_SIZE[1], 'view_mode': 'fit',
                'fps': FPS, 'validation': True, 'python': sys.executable}, revision)
            assert started['status'] == 'running', started
            expected = int(DURATION * FPS + 0.5)
            assert started['frames'] == expected, \
                f'the editor planned {started["frames"]} frames, expected {expected}: {started}'
            deadline = time.monotonic() + BRIDGE_JOB_TIMEOUT
            while True:
                job = bridge.request('get_job', {'kind': 'export'})
                if job['status'] != 'running':
                    break
                assert time.monotonic() < deadline, f'the baseline export never finished: {job}'
                time.sleep(.05)
            assert job['status'] == 'completed', job
            job_folder = data / 'build' / 'jobs' / job['id']
            snapshot = job_folder / 'snapshot.json'
            assert snapshot.is_file(), f'the editor wrote no baseline snapshot: {job_folder}'
            snapshot_bytes = snapshot.read_bytes()
            worker_log = job_folder / 'worker.log'
            worker_text = worker_log.read_text(encoding='utf-8') if worker_log.is_file() else ''
            worker_summary = validation_summary(worker_text)
            assert worker_summary == (0, 0), \
                f'the baseline export was not validation clean: {worker_summary}; {worker_log}'
            baseline = {'jobId': job['id'], 'frames': started['frames'],
                        'workerValidation': worker_summary, 'workerLog': str(worker_log)}
            # A dirty project keeps the private editor's window open; save first so
            # close() can shut this session (and only this session) down.
            try:
                bridge.request('save_project', {'path': str(folder / 'shutdown.physproj'),
                                                'expectedToken': bridge.request('get_project')['token'],
                                                'overwrite': True})
            except Exception:  # noqa: BLE001 - shutdown is best effort, never hides a failure
                pass
        finally:
            bridge.close()
    target = work / 'snapshot.json'
    target.write_bytes(snapshot_bytes)
    parsed = read_json(target)
    assert isinstance(parsed, dict), f'the copied snapshot is not JSON: {target}'
    assert parsed.get('fps') == FPS, f'the snapshot carries fps={parsed.get("fps")!r}, expected {FPS}'
    assert parsed.get('scene', {}).get('duration') == DURATION, \
        f'the snapshot scene duration is {parsed.get("scene", {}).get("duration")!r}, expected {DURATION}'
    assert parsed.get('canvasWidth') and parsed.get('canvasHeight'), \
        f'the snapshot carries no usable canvas geometry: {sorted(parsed)}'
    return target, baseline


def run_child(executable, request_path, run_dir, log_path, timeout):
    """Run the canonical ``--export-job`` CLI in the test's own directory."""
    argv = [str(executable), '--export-job', str(request_path), '--validation']
    started = time.monotonic()
    process = subprocess.Popen(argv, cwd=str(run_dir), env=child_env(),
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT,
                               creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    try:
        output = process.communicate(timeout=timeout)[0]
        timed_out = False
    except subprocess.TimeoutExpired:
        # A hung export is the defect this test is about: kill this child and
        # report the timeout instead of waiting forever.
        process.kill()
        output = process.communicate()[0]
        timed_out = True
    elapsed = time.monotonic() - started
    text = (output or b'').decode('utf-8', 'replace')
    log_path.write_text(text, encoding='utf-8')
    return {'argv': argv, 'code': process.returncode, 'log': text,
            'seconds': elapsed, 'timedOut': timed_out}


def run_child_monitored(executable, request_path, run_dir, frames_dir, log_path, timeout):
    """Like ``run_child``, but samples the frame directory's progress while it runs.

    Reading the published progress *before* counting the files keeps the invariant
    meaningful: a frame is counted only after ``png::write`` returned, so the
    published ``completedFrames`` can never exceed the files already on disk.
    """
    samples = []
    stop = threading.Event()

    def sample():
        while not stop.wait(0.003):
            payload = read_json(frames_dir / 'progress.json')
            if isinstance(payload, dict):
                payload['filesOnDisk'] = len(frame_files(frames_dir))
                samples.append(payload)

    reader = threading.Thread(target=sample, name='offscreen-progress-sampler', daemon=True)
    reader.start()
    try:
        outcome = run_child(executable, request_path, run_dir, log_path, timeout)
    finally:
        stop.set()
        reader.join(timeout=2.0)
    outcome['samples'] = samples
    return outcome


def failure_scenario(executable, work, name, frames, blocked_index, size, snapshot):
    """One export whose ``blocked_index``-th frame cannot reach the disk."""
    run_dir = work / name
    request_path, frames_dir = write_request(run_dir, snapshot, frames, size)
    frames_dir.mkdir(parents=True, exist_ok=True)
    blocked = frames_dir / (FRAME_PATTERN % blocked_index)
    # The deterministic trigger: the PNG writer must open this path as a file, and
    # opening an existing directory for writing fails on Windows.
    blocked.mkdir()
    outcome = run_child(executable, request_path, run_dir, run_dir / 'native.log', FAILURE_TIMEOUT)
    where = f'{run_dir}'

    assert not outcome['timedOut'], (
        f'the exporter hung for more than {FAILURE_TIMEOUT:.0f}s after frame {blocked_index} could not be '
        f'written (a dead encoder pool must fail the export, not block it); {where}')
    assert outcome['code'] != 0, (
        f'the export exited 0 even though frame {blocked_index} could not be written; {where}')
    assert any(marker in outcome['log'] for marker in PNG_FAILURE_MARKERS), (
        f'the failure was not reported as a PNG write error (markers {PNG_FAILURE_MARKERS}): '
        f'{outcome["log"][-2000:]}; {where}')
    assert_clean_validation(outcome['log'], where, require_summary=False)

    result_file = frames_dir / 'result.json'
    if result_file.exists():
        payload = read_json(result_file)
        assert isinstance(payload, dict) and payload.get('ok') is not True, (
            f'a failed export announced success in result.json: {payload!r}; {where}')

    # Every frame before the blocked one was encoded and written; the blocked one
    # and everything after it must not exist. This also covers the documented
    # "drain pending frames on the error path" contract of stopEncoders().
    files = frame_files(frames_dir)
    expected = [FRAME_PATTERN % index for index in range(blocked_index)]
    assert [item.name for item in files] == expected, (
        f'expected frames {expected} on disk after the failure, found '
        f'{[item.name for item in files]}; {where}')
    for item in files:
        assert item.stat().st_size > len(PNG_SIGNATURE) and item.read_bytes()[:8] == PNG_SIGNATURE, \
            f'{item} is not a complete PNG; {where}'
    assert blocked.is_dir(), f'the blocking directory was replaced: {blocked}; {where}'

    progress = read_json(frames_dir / 'progress.json')
    if progress is not None:
        assert isinstance(progress, dict), f'the published progress is not an object: {progress!r}; {where}'
        assert progress.get('stage') == 'render', f'unexpected progress stage: {progress}; {where}'
        assert progress.get('totalFrames') == frames, f'wrong totalFrames: {progress}; {where}'
        # The forced final snapshot is only published after waitForPendingWrites(),
        # so a failed export keeps an advisory in-flight sample: it may lag, but it
        # must never claim the blocked frame.
        assert progress.get('completedFrames', 0) <= blocked_index, (
            f'a failed export counted the blocked frame: {progress}; {where}')
        assert progress.get('completedFrames', 0) <= len(files), (
            f'the counters claim more frames than exist on disk: {progress} vs {len(files)}; {where}')

    return {'name': name, 'frames': frames, 'blockedIndex': blocked_index, 'size': list(size),
            'exitCode': outcome['code'], 'seconds': round(outcome['seconds'], 3),
            'framesOnDisk': [item.name for item in files], 'resultExists': result_file.exists(),
            'progress': progress, 'validationErrors': validation_errors(outcome['log']),
            'log': str(run_dir / 'native.log')}


def success_scenario(executable, work, index, frames, size, snapshot):
    """A short successful sequence must publish exactly the frames it wrote."""
    name = f'success-{index:02d}'
    run_dir = work / name
    request_path, frames_dir = write_request(run_dir, snapshot, frames, size)
    outcome = run_child_monitored(executable, request_path, run_dir, frames_dir,
                                  run_dir / 'native.log', SUCCESS_TIMEOUT)
    where = f'{run_dir}'

    assert not outcome['timedOut'], f'the short export did not finish in {SUCCESS_TIMEOUT:.0f}s; {where}'
    assert outcome['code'] == 0, f'the short export exited {outcome["code"]}: {outcome["log"][-2000:]}; {where}'
    summary = assert_clean_validation(outcome['log'], where, require_summary=True)

    result = read_json(frames_dir / 'result.json')
    assert isinstance(result, dict) and result.get('ok') is True, f'missing ok result.json: {result!r}; {where}'
    assert result.get('frames') == frames and result.get('width') == size[0] \
        and result.get('height') == size[1], f'result.json disagrees with the request: {result}; {where}'

    files = frame_files(frames_dir)
    assert len(files) == frames, f'expected {frames} files, found {len(files)}: {files}; {where}'
    for item in files:
        assert item.stat().st_size > len(PNG_SIGNATURE) and item.read_bytes()[:8] == PNG_SIGNATURE, \
            f'{item} is not a complete PNG; {where}'

    progress = read_json(frames_dir / 'progress.json')
    assert isinstance(progress, dict), f'the export published no progress.json: {where}'
    assert progress.get('stage') == 'render', f'unexpected final stage: {progress}; {where}'
    assert progress.get('totalFrames') == frames, f'wrong totalFrames: {progress}; {where}'
    # The regression: the completion counter is published in the same critical
    # section that releases the staging slot, so the final forced snapshot taken
    # after waitForPendingWrites() can never lag the files actually written.
    assert progress.get('completedFrames') == frames, (
        f'the final progress reports {progress.get("completedFrames")} of {frames} frames while '
        f'{len(files)} are on disk: {progress}; {where}')

    live = [item for item in outcome['samples'] if isinstance(item.get('completedFrames'), int)]
    assert live, f'no live progress sample was observed during a {frames}-frame export; {where}'
    counts = [item['completedFrames'] for item in live]
    assert counts == sorted(counts), f'the published frame counter went backwards: {counts}; {where}'
    assert all(0 <= count <= frames for count in counts), f'counter out of range: {counts}; {where}'
    assert all(item['completedFrames'] <= item.get('filesOnDisk', 0) for item in live), (
        f'the published counter ran ahead of the files on disk: '
        f'{[(i["completedFrames"], i.get("filesOnDisk")) for i in live]}; {where}')

    return {'name': name, 'frames': frames, 'size': list(size),
            'seconds': round(outcome['seconds'], 3), 'samples': len(live),
            'distinctCounts': sorted({item['completedFrames'] for item in live}),
            'finalProgress': progress, 'validation': summary, 'log': str(run_dir / 'native.log')}


def run(executable, work_root=None):
    executable = Path(executable).resolve()
    assert executable.is_file(), f'the canonical editor executable does not exist: {executable}'
    parent = Path(work_root).resolve() if work_root else DEFAULT_WORK_PARENT
    work = parent / ('offscreen-failure-' + uuid.uuid4().hex)
    work.mkdir(parents=True)

    snapshot, baseline = acquire_snapshot(executable, work)
    report = {'executable': str(executable), 'work': str(work), 'baseline': baseline,
              'failures': [], 'successes': []}

    # --- 1. the only frame fails: the smallest deterministic window for the
    # waitForPendingWrites()-ignores-encodeFailure defect. A single frame also
    # means "all slots free" is true the instant the failed encoder returns, which
    # is exactly the state the old code mistook for success.
    report['failures'].append(failure_scenario(
        executable, work, 'failure-single', frames=1, blocked_index=0,
        size=FAILURE_SIZE, snapshot=snapshot))

    # --- 2. the last of four frames fails: earlier frames must still reach the
    # disk (stopEncoders drains on the error path) while the export still fails
    # and still refuses to announce success.
    report['failures'].append(failure_scenario(
        executable, work, 'failure-last-frame', frames=4, blocked_index=3,
        size=FAILURE_SIZE, snapshot=snapshot))

    # --- 3. successful short sequences, repeated: the concurrency window at the
    # end of an export is where the counter/slot ordering matters, and every repeat
    # must end at completedFrames == totalFrames == files on disk.
    for index in range(SUCCESS_REPEATS):
        report['successes'].append(success_scenario(
            executable, work, index, frames=SUCCESS_FRAMES, size=SUCCESS_SIZE, snapshot=snapshot))

    write_json(work / 'report.json', report)
    successes = report['successes']
    print(f'PASS: native offscreen failure path; frames=1 and last-frame-of-4 both exit '
          f'{[item["exitCode"] for item in report["failures"]]} with no ok result.json, no Vulkan '
          f'resource errors and a bounded wall clock; {len(successes)}x{SUCCESS_FRAMES}-frame successes '
          f'end at completedFrames == {SUCCESS_FRAMES} == files on disk '
          f'(samples {[item["samples"] for item in successes]}); {work}')


def main(argv):
    if len(argv) < 1 or len(argv) > 2:
        print('usage: python mcp/test_offscreen_failure.py <physics_editor.exe> [work-dir]')
        return 2
    run(argv[0], argv[1] if len(argv) > 1 else None)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
