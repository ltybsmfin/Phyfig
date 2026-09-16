"""Live native regression: runtime formulas, portable masks and isolated exports."""
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid
import zlib
from bridge import EditorBridge

ROOT=Path(__file__).resolve().parents[1]
user32=ctypes.windll.user32
user32.SetWindowPos.argtypes=[wintypes.HWND,wintypes.HWND,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,wintypes.UINT]

def png(path):
    data=path.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n'
    w,h=struct.unpack('>II',data[16:24]);offset=8;compressed=b''
    while offset<len(data):
        n=struct.unpack('>I',data[offset:offset+4])[0];kind=data[offset+4:offset+8];chunk=data[offset+8:offset+8+n]
        if kind==b'IDAT':compressed+=chunk
        offset+=12+n
    raw=zlib.decompress(compressed);assert len(raw)==h*(w*4+1)
    return w,h,raw

def windows(pid):
    result=[]
    callback=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
    @callback
    def visit(hwnd,_):
        owner=wintypes.DWORD();user32.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        if owner.value==pid and user32.IsWindowVisible(hwnd):result.append(hwnd)
        return True
    user32.EnumWindows(visit,0);return result

def wait_job(bridge,kind,timeout=100):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        result=bridge.request('get_job',{'kind':kind})
        if result['status']!='running':return result
        time.sleep(.08)
    raise AssertionError(f'{kind} job timed out')

def editor_pids(image):
    try:
        output=subprocess.check_output(['tasklist','/NH','/FO','CSV','/FI',f'IMAGENAME eq {image}'],
            text=True,errors='replace',timeout=20)
    except (OSError,subprocess.SubprocessError):
        return None
    ids=set()
    for line in output.splitlines():
        fields=line.split('","')
        if len(fields)>=2 and fields[0].strip('"').lower()==image.lower():ids.add(fields[1])
    return ids

def verify_stdio_server(executable):
    """Delta 2: server.py --editor-transport stdio starts exactly one private editor.

    The main flow above uses named pipes; this exercises the explicit stdio
    transport end to end (server -> its own child editor) and checks the child is
    reaped when the MCP session closes.
    """
    import asyncio
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    baseline=editor_pids(executable.name)
    async def scenario():
        server=StdioServerParameters(command=sys.executable,args=[str(ROOT/'mcp/server.py'),
            '--editor-transport','stdio','--start-editor',str(executable),'--workspace',str(ROOT)])
        async with stdio_client(server) as (read,write):
            async with ClientSession(read,write) as session:
                await session.initialize()
                result=await session.call_tool('get_scene',{})
                assert not result.isError,result
                scene=json.loads(next(c.text for c in result.content if c.type=='text'))
                assert scene['revision']==0,scene
    asyncio.run(scenario())
    if baseline is not None:
        deadline=time.monotonic()+15
        current=editor_pids(executable.name)
        while time.monotonic()<deadline and current is not None and not current<=baseline:
            time.sleep(.2);current=editor_pids(executable.name)
        assert current is not None and current<=baseline,f'stdio editor was not reaped: {current-baseline}'
    print('PASS: --editor-transport stdio starts one private editor and closes only it')

def run(executable,formula_python):
    folder=ROOT/'build/verification'/('runtime-'+uuid.uuid4().hex);folder.mkdir(parents=True)
    # Delta 1: the editor is started from an unrelated working directory with an
    # explicit writable data root. Journals and job scratch must land under the
    # data root, and tools/assets must still resolve from the discovered
    # resource root (the source tree), never from the process cwd.
    cwd=folder/'cwd';cwd.mkdir(exist_ok=True)
    data=folder/'data'
    def launch(project=None):
        pipe='runtime-'+uuid.uuid4().hex;log=(folder/(pipe+'.log')).open('w',encoding='utf-8')
        args=[str(executable),'--validation','--pipe',pipe,'--data-dir',str(data)]
        if project:args+=['--project',str(project)]
        process=subprocess.Popen(args,cwd=cwd,stdout=log,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW,
            env={**os.environ,'VK_LAYER_VALIDATE_SYNC':'1'})
        bridge=EditorBridge(pipe)
        try:bridge.request('get_scene')
        except BaseException:process.terminate();process.wait();log.close();raise
        return process,bridge,log
    process,bridge,log=launch()
    def close_cleanly(process,bridge):
        bridge.request('save_project',{'path':str(folder/f'shutdown-{process.pid}.physproj'),'expectedToken':bridge.request('get_project')['token']})
        for hwnd in windows(process.pid):user32.PostMessageW(hwnd,0x0010,0,0)
        assert process.wait(timeout=15)==0,'Native shutdown failed'
    try:
        scene=bridge.request('get_scene');scene=bridge.request('apply',{'commands':[{'op':'settings','duration':1}]},scene['revision'])
        baseline=folder/'baseline.physproj';bridge.request('save_project',{'path':str(baseline),'expectedToken':bridge.request('get_project')['token']})
        source=r'\vec{v}_0=\frac{\Delta x}{\Delta t}'
        job=bridge.request('set_formula',{'latex':source,'id':'runtime-formula','python':formula_python,'x':1,'y':9},scene['revision'])
        # Opening an unrelated draft must not cancel a scene formula compilation.
        bridge.request('open_component',{'id':'independent_draft'})
        compiled=wait_job(bridge,'formula');assert compiled['status']=='completed',compiled
        assert compiled['id']==job['id'];scene=bridge.request('get_scene')
        asset=next(o for o in scene['document']['objects'] if o['id']=='runtime-formula')['properties']['formula']
        assert asset in bridge.request('get_formulas')['assets']
        bridge.request('close_component',{'discard':True},scope='component')
        time.sleep(.2);view=bridge.request('get_view_state');assert view['presentedRevision']==scene['revision'],view
        bridge.request('undo',revision=scene['revision']);scene=bridge.request('get_scene')
        assert not any(o['id']=='runtime-formula' for o in scene['document']['objects'])
        bridge.request('redo',revision=scene['revision']);scene=bridge.request('get_scene')
        revision=scene['revision']
        bridge.request('set_formula',{'latex':r'\input{a}','id':'runtime-formula','python':formula_python},revision)
        assert wait_job(bridge,'formula')['status']=='failed';assert bridge.request('get_scene')['revision']==revision
        # A document edit while compilation runs must not get overwritten.
        bridge.request('set_formula',{'latex':r'a=\sqrt{b^2+c^2}','id':'runtime-formula','python':formula_python},revision)
        scene=bridge.request('apply',{'commands':[{'op':'move','id':'runtime-formula','dx':1,'dy':0}]},revision)
        assert wait_job(bridge,'formula')['status']=='failed';assert bridge.request('get_scene')['revision']==scene['revision']
        bridge.request('set_formula',{'latex':r'x^2','id':'runtime-formula','python':'python'},scene['revision'])
        # Default Python may lack pypdfium2; both successful configured installs and clear failures are legitimate.
        default=wait_job(bridge,'formula');assert default['status'] in ('completed','failed')
        scene=bridge.request('get_scene')
        project=folder/'portable.physproj';token=bridge.request('get_project')['token']
        bridge.request('save_project',{'path':str(project),'expectedToken':token})
        # The runtime formula carries its structural writing plan into the project:
        # it names every ink component of the stored mask exactly once, and the
        # fraction bar comes before the units above and below it.
        package=json.loads(project.read_text(encoding='utf-8'))
        stored=next(a for a in package['resources']['formulaMasks'] if a['name']==asset)
        plan=stored['writingPlan']
        assert plan['version']==1 and plan['componentCount']>0 and plan['units'],stored.get('name')
        assert (plan['width'],plan['height'])==(stored['width'],stored['height']),stored.get('name')
        named=[c for unit in plan['units'] for c in unit['components']]
        assert sorted(named)==list(range(plan['componentCount'])),'writing plan must name every component exactly once'
        assert all(unit['components'] for unit in plan['units'])
        boxes=[unit['box'] for unit in plan['units']]
        bars=[index for index,unit in enumerate(plan['units']) if unit['kind']=='rule']
        assert bars,'the runtime formula must plan at least one rule line'
        for bar in bars:
            box=boxes[bar]
            operands=[index for index,other in enumerate(boxes) if index!=bar
                and other[0]>=box[0]-2 and other[2]<=box[2]+2
                and (other[3]<=box[1]+.5 or other[1]>=box[3]-.5)]
            if not operands:continue
            assert all(bar<index for index in operands),f'fraction bar {bar} must precede {operands}'
        # All export modes share the frozen document and camera.
        for format,fps,width,height in [('png',4,1280,720),('mp4',6,320,180),('gif',6,320,180)]:
            output=folder/('frames' if format=='png' else 'animation.'+format)
            revision=bridge.request('get_scene')['revision']
            job=bridge.request('export_animation',{'output':str(output),'format':format,'fps':fps,'width':width,'height':height,'validation':True},revision)
            if format=='png':
                for hwnd in windows(process.pid):
                    user32.SetWindowPos(hwnd,None,0,0,1300,880,0x0002|0x0004)
                    user32.ShowWindow(hwnd,6)
                bridge.request('apply',{'commands':[{'op':'move','id':'runtime-formula','dx':5,'dy':0}]},revision)
                for hwnd in windows(process.pid):user32.ShowWindow(hwnd,9)
            done=wait_job(bridge,'export');assert done['status']=='completed',done
            assert done['id']==job['id'] and output.exists(),done
            native_log=(data/'build/jobs'/done['id']/'worker.log').read_text(encoding='utf-8')
            assert 'Validation errors: 0; warnings: 0' in native_log,native_log[-4000:]
            if format=='png':
                files=sorted(output.glob('frame_*.png'));assert len(files)==fps
                assert all(png(p)[:2]==(width,height) for p in files)
                assert png(files[0])[2]!=png(files[-1])[2],'Animation frames unexpectedly identical'
            else:
                probe=subprocess.check_output(['ffprobe','-v','error','-show_entries','stream=width,height:format=duration','-of','json',str(output)])
                metadata=json.loads(probe);assert metadata['streams'][0]['width']==width and metadata['streams'][0]['height']==height
                assert abs(float(metadata['format']['duration'])-1)<=1/fps,metadata
        # A1: the presented-frame viewport is reported and the live view is separate.
        revision=bridge.request('get_scene')['revision']
        view=bridge.request('get_view_state')
        assert view['canvasReady'] and view['canvasWidth']>0 and view['canvasHeight']>0,view
        assert view['viewportSource']=='presented' and view['presented']['source']=='presented' and view['view']['source']=='live',view
        assert view['presentedRevision']==revision and len(view['origin'])==2 and view['zoom']>0,view
        # A2/A3: fit uses an independent viewport for the requested size and the
        # target scope; current can match the recorded canvas, which stays even.
        fit_frames=folder/'fit-frames'
        bridge.request('export_animation',{'output':str(fit_frames),'format':'png','fps':4,'width':640,'height':360,'view_mode':'fit','validation':True},revision)
        done=wait_job(bridge,'export');assert done['status']=='completed',done
        assert done['viewMode']=='fit' and done['width']==640 and done['height']==360,done
        assert done['viewport']['mode']=='fit' and done['viewport']['scope']=='scene',done
        assert all(png(p)[:2]==(640,360) for p in sorted(fit_frames.glob('frame_*.png')))
        match_frames=folder/'match-frames'
        revision=bridge.request('get_scene')['revision']
        bridge.request('export_animation',{'output':str(match_frames),'format':'png','fps':2,'width':0,'height':0,'view_mode':'current','validation':True},revision)
        done=wait_job(bridge,'export');assert done['status']=='completed',done
        assert done['viewMode']=='current' and done['width']>=16 and done['height']>=16 and done['width']%2==0 and done['height']%2==0,done
        assert all(png(p)[:2]==(done['width'],done['height']) for p in sorted(match_frames.glob('frame_*.png')))
        revision=bridge.request('get_scene')['revision']
        try:
            bridge.request('export_animation',{'output':str(folder/'implicit.mp4'),'view_mode':'fit','width':0,'height':0},revision)
            raise AssertionError('fit accepted width=height=0 instead of requiring explicit sizes')
        except ValueError as error:
            assert 'explicit' in str(error),error
        revision=bridge.request('get_scene')['revision']
        bridge.request('export_animation',{'output':str(folder/'missing.mp4'),'ffmpeg':'missing-physics-ffmpeg.exe'},revision)
        failed=wait_job(bridge,'export');assert failed['status']=='failed',failed
        assert 'ffmpeg' in failed.get('error','').lower(),failed
        assert (data/'build/jobs'/failed['id']/'worker.log').is_file(),failed
        assert bridge.request('get_scene')['revision']==revision and process.poll() is None
        bridge.request('apply',{'commands':[{'op':'settings','duration':30}]},revision);revision=bridge.request('get_scene')['revision']
        job=bridge.request('export_animation',{'output':str(folder/'cancel.mp4'),'fps':24},revision)
        result=bridge.request('cancel_job',{'kind':'export','id':job['id']});assert result['status']=='cancelled'
        assert process.poll() is None
        token=bridge.request('get_project')['token'];bridge.request('load_project',{'path':str(project),'expectedToken':token,'discard':True})
        # Component documents have a different kind and need their own export context.
        bridge.request('open_component',{'id':'export_draft'})
        draft=bridge.request('get_scene',scope='component')
        bridge.request('apply',{'commands':[{'op':'settings','duration':.2},{'op':'create','id':'test-point','type':'point'}]},draft['revision'],scope='component')
        time.sleep(.15);draft=bridge.request('get_scene',scope='component')
        bridge.request('export_animation',{'output':str(folder/'component-frames'),'format':'png','fps':5,'width':320,'height':180,'validation':True},draft['revision'],scope='component')
        done=wait_job(bridge,'export');assert done['status']=='completed',done
        assert png(folder/'component-frames/frame_000000.png')[:2]==(320,180)
        close_cleanly(process,bridge)
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=10)
        log.close()
    process,bridge,log=launch(project)
    try:
        scene=bridge.request('get_scene');assert any(o['id']=='runtime-formula' for o in scene['document']['objects'])
        assert asset in bridge.request('get_formulas')['assets'];assert not scene['playback']['playing']
        bridge.request('undo',revision=scene['revision'])
        token=bridge.request('get_project')['token'];bridge.request('load_project',{'path':str(baseline),'expectedToken':token,'discard':True})
        assert not any(name.startswith('latex_') for name in bridge.request('get_formulas')['assets']),'Custom assets leaked across projects'
        close_cleanly(process,bridge)
    finally:
        if process.poll() is None:process.terminate();process.wait(timeout=10)
        log.close()
    for path in folder.glob('*.log'):
        text=path.read_text(encoding='utf-8');assert '[Vulkan]' not in text and 'Validation errors: 0; warnings: 0' in text,text[-4000:]
        # The explicit data root is authoritative and resources resolved from the
        # source tree even though the editor ran from an unrelated directory.
        assert '[runtime] data=' in text and str(data) in text,text[-2000:]
        assert 'resources=<not found>' not in text,text[-2000:]
    verify_stdio_server(executable)
    print('PASS: runtime LaTeX, rejection/conflict/undo, portable resource restart, PNG/MP4/GIF timing, resize/minimize, encoder failure, cancellation, cwd-independent data/resource roots')
    print('Artifacts:',folder)

if __name__=='__main__':run(Path(sys.argv[1]).resolve(),sys.argv[2])
