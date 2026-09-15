"""Real Vulkan regression for Write entry, including the transition to normal text.

Uses a private stdio editor and isolated export worker; never attaches to a user's
window. Endpoints alone cannot detect a discontinuous animation, so compare the
last nearly-complete frame against the normal renderer as well.
"""
import json
import os
from pathlib import Path
import sys
import time
import uuid

from bridge import StdioEditorBridge
from test_runtime_assets import png

ROOT = Path(__file__).resolve().parents[1]


def run(executable):
    folder = ROOT / 'build/verification' / ('write-render-' + uuid.uuid4().hex)
    folder.mkdir(parents=True)
    scene = json.loads((ROOT / 'scenes/write-entry.json').read_text(encoding='utf-8'))
    scene['duration'] = .2
    for obj in scene['objects']:
        if 'write' in obj:
            obj['write'].update(start=0, duration=.100001)
    native_log = folder / 'native.log'
    with native_log.open('w', encoding='utf-8') as log:
        bridge = StdioEditorBridge(executable, args=['--validation', '--data-dir', str(folder/'data')],
            cwd=ROOT, env={**os.environ, 'VK_LAYER_VALIDATE_SYNC': '1'}, stderr_sink=log)
        try:
            state = bridge.request('get_scene')
            bridge.request('replace', {'document': scene}, state['revision'])
            revision = bridge.request('get_scene')['revision']
            for target in (.1, .05, 0.0):
                bridge.request('playback', {'time': target, 'playing': False, 'step': 0})
                deadline = time.monotonic() + 8
                while True:
                    view = bridge.request('get_view_state')
                    if view.get('presentedTime') == target and view.get('presentedRevision') == revision:
                        break
                    assert time.monotonic() < deadline, view
                    time.sleep(.05)
                assert view['visible'] and view['canvasReady'] and not view['minimized'], view
                assert bridge.request('get_scene')['revision'] == revision

            frames = folder / 'frames'
            bridge.request('export_animation', {'output': str(frames), 'format': 'png', 'fps': 20,
                'width': 1280, 'height': 720, 'view_mode': 'fit', 'validation': True}, revision)
            deadline = time.monotonic() + 90
            while True:
                job = bridge.request('get_job', {'kind': 'export'})
                if job['status'] != 'running':
                    break
                assert time.monotonic() < deadline, job
                time.sleep(.1)
            assert job['status'] == 'completed', job
            files = sorted(frames.glob('frame_*.png'))
            assert len(files) == 4, files
            images = [png(path) for path in files]
            assert all(item[:2] == (1280, 720) for item in images)
            # Frame 0 has no writing; frame 1 is visibly in progress.
            assert images[0][2] != images[1][2] != images[3][2]
            before, after = images[2][2], images[3][2]
            max_delta = max(abs(a-b) for a, b in zip(before, after))
            changed = sum(a != b for a, b in zip(before, after))
            report = {'completionMaxChannelDelta': max_delta, 'completionChangedChannels': changed,
                'job': job, 'frames': str(frames)}
            (folder/'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            # Allow only two 8-bit levels for GPU interpolation/rounding, not a
            # relaxed percentage of the canvas that could hide displaced letters.
            assert max_delta <= 2, f'Write completion jumps: max delta {max_delta}, {changed} changed channels; {folder}'
            worker_log = folder/'data/build/jobs'/job['id']/'worker.log'
            assert 'Validation errors: 0; warnings: 0' in worker_log.read_text(encoding='utf-8')
            project = bridge.request('get_project')
            bridge.request('save_project', {'path': str(folder/'scene.physproj'),
                'expectedToken': project['token'], 'overwrite': True})
        finally:
            bridge.close()
        assert bridge.process.returncode == 0, bridge.stderr_text()
    assert 'Validation errors: 0; warnings: 0' in native_log.read_text(encoding='utf-8')
    print(f'PASS: live forward/backward seek, four exported frames, completion delta {max_delta}, Vulkan validation 0/0; {folder}')


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve())
