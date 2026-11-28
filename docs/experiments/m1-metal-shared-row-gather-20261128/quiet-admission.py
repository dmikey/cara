import sys,json,time,runpy
from pathlib import Path
sys.dont_write_bytecode=True
sys.path.insert(0,'/Users/derekanderson/Documents/Codex/2026-11-02/can-you-get-the-latest-citra-3/outputs/cara-m1-jit-metal-current-20261125')
import replay_dual as d
import host_gate_full as host
root=Path(__file__).resolve().parent
ready=root/'ROW-GATHER-RENDERER-SOURCE-READY.json'
release=root/'ROW-GATHER-RENDERER-RELEASE.json'
assert ready.exists() and not release.exists()
assert not (root/'row-gather-renderer-matched-1').exists()
checks=[]
for i in range(3):
    d.base.quiet();v=host.snapshot();checks.append(v)
    if v['total_cpu_percent']>50:break
    if i<2:time.sleep(3)
passed=len(checks)==3 and all(v['total_cpu_percent']<=50 for v in checks)
admission={'passed':passed,'checks':checks,'native_started':False,'source_ready_sha256':d.sha(ready)}
index=1
while (root/f'ROW-GATHER-RENDERER-ADMISSION-{index}.json').exists():index+=1
path=root/f'ROW-GATHER-RENDERER-ADMISSION-{index}.json';d.write(path,admission)
if not passed:
    print('BLOCKED_BEFORE_RELEASE: background services not quiet; no native job/case/release created',flush=True)
    raise SystemExit(0)
# Exact identity preflight must finish before release, not merely before Popen.
v=d.read(ready);source=Path('/Volumes/Fast/CaraBuild/cara-m1-jit-metal-current-20261125')
assert d.sha(root/'matched_row_gather_renderers.py')==v['runner_sha256']
for p,h in v['harness_hashes'].items():assert d.sha(Path(p))==h
revision,binaries,audits=d.signin(source/'work/m1-jit-dual',source);assert revision==v['revision']
for k,b in binaries.items():assert d.sha(b)==v['binaries'][k]['sha256']
for p,h in v['source_hashes'].items():assert d.sha(source/p)==h
for p,h in v['gate_hashes'].items():assert d.sha(root/p)==h and d.read(root/p)['passed']
d.base.ROM=Path(v['rom']['path']);assert list(d.base.rom_identity())==v['rom']['identity']
import hashlib
rom_hash=hashlib.sha256();last=time.monotonic()
with d.base.ROM.open('rb') as stream:
    for chunk in iter(lambda:stream.read(8*1024*1024),b''):
        rom_hash.update(chunk)
        if time.monotonic()-last>=.2:d.base.quiet();last=time.monotonic()
assert rom_hash.hexdigest()==v['rom']['sha256']
d.base.quiet();after=host.snapshot();admission['post_preflight_host']=after
if after['total_cpu_percent']>50:
    admission.update(passed=False,phase='preflight-background-changed');d.write(path,admission)
    print('BLOCKED_BEFORE_RELEASE: background services changed during preflight',flush=True)
    raise SystemExit(0)
assert d.sha(ready)==admission['source_ready_sha256']
with release.open('x') as stream:json.dump({'released':True,'source_ready_sha256':d.sha(ready),'one_bracket_only':True},stream,indent=2)

runpy.run_path(str(root/'matched_row_gather_renderers.py'),run_name='__main__')
