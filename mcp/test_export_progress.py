"""Live regression: export progress is visible through the MCP bridge while the job runs.

The editor's BackgroundJob publishes ``<job folder>/progress.json`` into
``get_job(kind='export').progress``; the same state object drives the GUI jobs
panel. This test therefore drives three real exports through a private stdio
editor (never a user's window) and checks the *process*, not just the result:

* a PNG export must show several monotonic, increasing frame samples while it is
  still running, must never claim 100% before the commit, and must end at
  ``stage='done'`` with ``overallFraction=1.0``;
* a real MP4 export must render and then encode (``stage='encode'`` observed on
  the way), commit a non-empty file, and reach 100% only at the end;
* cancelling keeps the last progress sample with the terminal ``cancelled`` stage,
  leaves no temporary frames or directory behind, and an editor that still answers
  requests;
* a failing encode keeps the last progress with ``stage='failed'`` and the worker's
  own reason, still below 100%, and the editor stays alive.

Every scenario also cross-checks the editor's operation log: the request is recorded
once, and each terminal state (completed / cancelled / failed) is appended as its own
``export.job`` record that keeps the MCP source, the scene scope and the job id.

Usage::

    python mcp/test_export_progress.py <physics_editor.exe> [work-dir]

The work directory defaults to ``build/verification`` (like the other live MCP
tests); a confined or read-only checkout can pass an explicit writable directory.
"""
import json
import os
from pathlib import Path
import sys
import time
import uuid

from bridge import StdioEditorBridge

ROOT = Path(__file__).resolve().parents[1]

PROGRESS_KEYS = ('version', 'stage', 'phase', 'completedFrames', 'totalFrames',
                 'elapsedSeconds', 'phaseFraction', 'overallFraction', 'estimateScope')
# Sampling-derived numbers. The worker only publishes them once a rate window
# exists, and BackgroundJob drops a non-number (including an explicit null) so a
# reader can never mistake "not measured" for a measurement. A 320x180 render can
# legitimately finish before the first window, so these stay optional here and are
# only required to be numbers when they are present.
PROGRESS_OPTIONAL_NUMBERS = ('fps', 'estimatedRemainingSeconds')

RENDER_STAGES = ('prepare', 'render')
DURATION = 3.0


def wait_terminal(bridge, kind, timeout, on_sample=None):
    """Polls get_job until the job stops running, recording every progress sample."""
    samples = []
    started = time.monotonic()
    deadline = started + timeout
    while True:
        state = bridge.request('get_job', {'kind': kind})
        progress = state.get('progress')
        if isinstance(progress, dict):
            samples.append(progress)
            if on_sample is not None:
                on_sample(progress)
        if state['status'] != 'running':
            return state, samples, time.monotonic() - started
        assert time.monotonic() < deadline, f'{kind} job did not finish: {state}'
        time.sleep(.03)


def check_monotonic(samples):
    frames = [item.get('completedFrames') for item in samples if 'completedFrames' in item]
    fractions = [item.get('overallFraction') for item in samples if 'overallFraction' in item]
    assert frames == sorted(frames), f'frame counter went backwards: {frames}'
    assert fractions == sorted(fractions), f'overall fraction went backwards: {fractions}'
    assert all(0.0 <= item <= 1.0 for item in fractions), fractions


def job_folder(data, job):
    return data / 'build' / 'jobs' / job['id']


def activity_records(bridge, method, expect=1, timeout=5.0):
    """Entries of the operation log with this method, waiting for ``expect`` of them.

    ``get_activity_log`` is a read-only query, so polling it never changes the log.
    Terminal job records are appended by the editor's own poll loop (one frame after
    the worker's state becomes terminal), hence the short wait.
    """
    deadline = time.monotonic() + timeout
    while True:
        page = bridge.request('get_activity_log', {'after': 0, 'limit': 200})
        found = [entry for entry in page['entries'] if entry['method'] == method]
        if len(found) >= expect or time.monotonic() >= deadline:
            return found
        time.sleep(.05)


def run(executable, work_root=None):
    work = Path(work_root or (ROOT / 'build/verification')) / ('export-progress-' + uuid.uuid4().hex)
    work.mkdir(parents=True)
    data = work / 'data'
    log = (work / 'native.log').open('w', encoding='utf-8')
    bridge = StdioEditorBridge(executable, args=['--validation', '--data-dir', str(data)], cwd=ROOT,
                               env={**os.environ, 'VK_LAYER_VALIDATE_SYNC': '1'}, stderr_sink=log)
    report = {'work': str(work), 'durationSeconds': DURATION, 'exports': {}}
    python = sys.executable
    try:
        state = bridge.request('get_scene')
        assert bridge.request('get_job', {'kind': 'export'})['status'] == 'idle'
        revision = bridge.request('apply', {'commands': [{'op': 'settings', 'duration': DURATION}]},
                                  state['revision'])['revision']
        revision = bridge.request('get_scene')['revision']

        # --- 1. PNG: many increasing samples while running, no encode stage ----
        frames = work / 'frames'
        started = bridge.request('export_animation', {
            'output': str(frames), 'format': 'png', 'width': 320, 'height': 180,
            'view_mode': 'fit', 'fps': 24, 'validation': True, 'python': python}, revision)
        total = started['frames']
        assert started['status'] == 'running', started
        # The GUI loop draws the jobs panel every frame; sampling get_view_state
        # while the export runs proves the editor window kept presenting (so the
        # panel really ran on the UI thread with live progress), not just that the
        # job state was readable.
        gui = {}

        def note(progress):
            if not gui and progress.get('completedFrames', 0) >= 1:
                gui.update(bridge.request('get_view_state'))

        job, samples, wall = wait_terminal(bridge, 'export', 300, on_sample=note)
        assert job['status'] == 'completed', job
        assert gui.get('visible') and gui.get('canvasReady'), \
            f'the editor window must keep presenting during an export: {gui}'
        check_monotonic(samples)
        running = [item for item in samples if item['stage'] != 'done']
        distinct = sorted({item['completedFrames'] for item in samples if 'completedFrames' in item})
        assert len(distinct) >= 2, f'no increasing samples while running: {samples}'
        assert all(item['stage'] in RENDER_STAGES + ('commit',) for item in running), running
        assert all(item['overallFraction'] < 1.0 for item in running), \
            f'a running export must never report 100%: {running}'
        assert job['progress']['stage'] == 'done' and job['progress']['overallFraction'] == 1.0, job['progress']
        assert job['progress']['completedFrames'] == total == len(list(frames.glob('frame_*.png')))
        for key in PROGRESS_KEYS:
            assert key in job['progress'], f'{key} missing from the published progress: {job["progress"]}'
        for key in PROGRESS_OPTIONAL_NUMBERS:
            assert key not in job['progress'] or isinstance(job['progress'][key], (int, float)), \
                f'{key} must be a number when it is published: {job["progress"]}'
        raw = json.loads((job_folder(data, job) / 'progress.json').read_text(encoding='utf-8'))
        assert raw['version'] == 1 and raw['stage'] == 'done', raw
        assert raw['completedFrames'] == raw['totalFrames'] == total, raw
        assert not (job_folder(data, job) / 'progress.json.tmp').exists(), 'a temp progress file was left behind'
        # renderSeconds/readbackWriteSeconds are the render worker's own telemetry:
        # when they are present the samples came from framesDirectory/progress.json
        # rather than from the fallback frame scan.
        source = 'native progress.json' if 'renderSeconds' in raw else 'frame scan fallback'
        # The operation log must show the request exactly once and the terminal outcome
        # as its own appended record: source/scope preserved, request never duplicated.
        requests = activity_records(bridge, 'export_animation')
        outcomes = activity_records(bridge, 'export.job')
        assert len(requests) == 1, requests
        assert len(outcomes) == 1, outcomes
        assert outcomes[0]['status'] == 'ok' and outcomes[0]['source'] == 'mcp', outcomes
        assert outcomes[0]['scope'] == 'scene' and outcomes[0]['error'] == '', outcomes
        assert outcomes[0]['detail']['job'] == job['id'], outcomes
        assert outcomes[0]['detail']['status'] == 'completed', outcomes
        report['exports']['png'] = {'job': job, 'distinctFrameSamples': distinct, 'samples': len(samples),
                                    'wallSeconds': round(wall, 3), 'rawProgress': raw, 'frames': total,
                                    'progressSource': source, 'guiWhileRunning': gui,
                                    'activityTerminal': outcomes[0]}

        # --- 2. MP4: a real render + encode, 100% only after the commit -------
        # 960x540 keeps the x264 pass long enough to be sampled by the editor's
        # 200 ms progress read; a 320p pass can finish inside a single read window.
        video = work / 'out.mp4'
        started = bridge.request('export_animation', {
            'output': str(video), 'format': 'mp4', 'width': 960, 'height': 540,
            'view_mode': 'fit', 'fps': 24, 'validation': True, 'python': python}, revision)
        assert started['status'] == 'running', started
        job, samples, wall = wait_terminal(bridge, 'export', 900)
        assert job['status'] == 'completed', job
        check_monotonic(samples)
        stages = [item['stage'] for item in samples]
        phases = [item['phase'] for item in samples]
        encode = [item for item in samples if item['stage'] == 'encode']
        assert encode, f'the encode stage was never observed: {stages}'
        assert all(item['overallFraction'] < 1.0 for item in samples if item['stage'] != 'done'), samples
        assert video.is_file() and video.stat().st_size > 0, video
        assert job['progress']['stage'] == 'done' and job['progress']['overallFraction'] == 1.0, job['progress']
        report['exports']['mp4'] = {'job': job, 'stages': stages, 'phases': phases,
                                    'encodeSamples': len(encode), 'wallSeconds': round(wall, 3),
                                    'bytes': video.stat().st_size}

        # --- 3. cancel keeps the last progress and the terminal status --------
        # 1080p at 60 fps is deliberately long: with the parallel readback/encoder
        # path a 640x360 run can finish inside one 200 ms progress read, and then
        # there is no cancel window left to observe (the job is already completed).
        scratch = work / 'cancel-frames'
        started = bridge.request('export_animation', {
            'output': str(scratch), 'format': 'png', 'width': 1920, 'height': 1080,
            'view_mode': 'fit', 'fps': 60, 'validation': True, 'python': python}, revision)
        cancel_total = started['frames']
        deadline = time.monotonic() + 60
        observed = None
        while observed is None:
            state = bridge.request('get_job', {'kind': 'export'})
            progress = state.get('progress')
            if isinstance(progress, dict) and progress.get('completedFrames', 0) >= 2:
                observed = progress
                break
            assert state['status'] == 'running', state
            assert time.monotonic() < deadline, 'the cancellable export never reported progress'
            time.sleep(.05)
        cancelled = bridge.request('cancel_job', {'kind': 'export', 'id': started['id']})
        assert cancelled['status'] == 'cancelled', cancelled
        kept = cancelled.get('progress')
        assert isinstance(kept, dict), f'cancel must keep the last progress: {cancelled}'
        assert 0 < kept['completedFrames'] <= cancel_total, kept
        assert kept['overallFraction'] < 1.0, kept
        # The terminal state is published by the editor from its own cancel, never by
        # the killed worker's last write: a cancelled job must not keep showing a
        # running stage, and the stale rate/estimate of the last live sample must be
        # gone instead of looking like the job still advances.
        assert kept['stage'] == 'cancelled' and kept['phase'] == 'cancelled', kept
        assert 'fps' not in kept, f'a cancelled job must drop the stale throughput: {kept}'
        assert kept.get('estimatedRemainingSeconds') is None, kept
        assert not scratch.exists(), 'a cancelled export must not commit anything'
        # The worker was killed, so its own "finally" never ran: the editor's
        # BackgroundJob must have removed the temporary directory it recorded,
        # together with every temporary frame inside it. A kept directory is a real
        # defect, and the record says exactly why it was kept - never silence.
        cleanup = cancelled.get('cleanup')
        assert isinstance(cleanup, dict) and cleanup.get('status'), \
            f'a cancelled export must report what happened to its temporary directory: {cancelled}'
        if cleanup['status'] != 'removed':
            raise AssertionError(
                'the cancelled export left its temporary directory (and its frames) behind: '
                + json.dumps(cleanup, ensure_ascii=False))
        assert not Path(cleanup['directory']).exists(), cleanup
        leftovers = sorted(path.name for path in work.glob('physics-export-*'))
        assert not leftovers, f'a cancelled export left temporary directories behind: {leftovers}'
        assert bridge.request('get_scene')['revision'] == revision, 'the editor died on cancel'
        # Cancel is a terminal state of its own: an appended warning record (never a
        # rewrite of the request row), plus the cancel_job request itself.
        outcomes = activity_records(bridge, 'export.job', expect=3)
        assert len(outcomes) == 3, outcomes
        assert outcomes[-1]['status'] == 'warning', outcomes[-1]
        assert outcomes[-1]['detail']['status'] == 'cancelled', outcomes[-1]
        assert outcomes[-1]['source'] == 'mcp' and outcomes[-1]['scope'] == 'scene', outcomes[-1]
        assert len(activity_records(bridge, 'export_animation', expect=3)) == 3
        assert len(activity_records(bridge, 'cancel_job')) == 1
        report['exports']['cancel'] = {'job': cancelled, 'observedBeforeCancel': observed,
                                       'activityTerminal': outcomes[-1]}

        # --- 4. a failing encode keeps the last progress and the editor alive --
        bad = work / 'bad.mp4'
        started = bridge.request('export_animation', {
            'output': str(bad), 'format': 'mp4', 'width': 320, 'height': 180,
            'view_mode': 'fit', 'fps': 8, 'validation': True, 'python': python,
            'ffmpeg': python}, revision)
        job, samples, wall = wait_terminal(bridge, 'export', 300)
        assert job['status'] == 'failed', job
        check_monotonic(samples)
        assert job['progress']['stage'] == 'failed', job['progress']
        assert job['progress']['overallFraction'] < 1.0, job['progress']
        assert job['progress'].get('message'), 'a failed progress must carry the reason'
        # The reason must be the worker's own, not just "it exited": the worker's last
        # progress write is best effort and may never be read, so the editor has to
        # build the terminal sample from result.json itself.
        result = json.loads((job_folder(data, job) / 'result.json').read_text(encoding='utf-8'))
        reason = result['error']['message']
        assert reason and reason in job['progress']['message'], \
            f'the failed progress must carry the worker reason {reason!r}: {job["progress"]}'
        assert not bad.exists(), 'a failed export must not commit the target'
        # The worker removes its own temporary directory on failure; if it could not,
        # the editor must report the concrete reason instead of leaving frames behind.
        leftovers = sorted(path.name for path in work.glob('physics-export-*'))
        assert not leftovers, (f'a failed export left temporary directories behind: {leftovers}; '
                               f'cleanup={job.get("cleanup")}')
        assert bridge.request('get_scene')['revision'] == revision, 'the editor died on a worker failure'
        # A worker failure is a visible terminal record carrying the reason.
        outcomes = activity_records(bridge, 'export.job', expect=4)
        assert len(outcomes) == 4, outcomes
        assert outcomes[-1]['status'] == 'error' and outcomes[-1]['error'], outcomes[-1]
        assert outcomes[-1]['detail']['status'] == 'failed', outcomes[-1]
        assert outcomes[-1]['detail']['job'] == job['id'], outcomes[-1]
        assert len(activity_records(bridge, 'export_animation', expect=4)) == 4
        report['exports']['failure'] = {'job': job, 'wallSeconds': round(wall, 3),
                                        'activityTerminal': outcomes[-1]}
    finally:
        # Shut the private editor down cleanly (an unsaved project would keep its
        # window open). A dead editor must never hide the real failure, so this
        # block is best effort and never replaces the original exception.
        try:
            bridge.request('save_project', {'path': str(work / 'shutdown.physproj'),
                                            'expectedToken': bridge.request('get_project')['token'],
                                            'overwrite': True})
        except Exception:  # noqa: BLE001
            pass
        bridge.close()
        log.close()
    (work / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    png = report['exports']['png']
    mp4 = report['exports']['mp4']
    print(f'PASS: live export progress; PNG {png["samples"]} samples / {len(png["distinctFrameSamples"])} '
          f'increasing frame counts -> done 100%, MP4 stages {mp4["stages"]} ({mp4["encodeSamples"]} encode '
          f'samples) -> {mp4["bytes"]} bytes, cancel keeps the last progress and removes every temporary frame, '
          f'failure keeps the last progress with the worker reason and the editor alive; {work}')


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve(), sys.argv[2] if len(sys.argv) > 2 else None)
