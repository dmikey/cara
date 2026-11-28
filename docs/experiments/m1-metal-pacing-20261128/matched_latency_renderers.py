#!/usr/bin/env python3
"""One reviewed GL/Metal/Metal/GL bracket; no retry or policy relaxation."""
import json,re,signal,sys,time
from pathlib import Path
sys.dont_write_bytecode=True
sys.path.insert(0,'/Users/derekanderson/Documents/Codex/2026-11-02/can-you-get-the-latest-citra-3/outputs/cara-m1-jit-metal-current-20261125')
import replay_dual as d
import host_load_observer as host
root=Path(__file__).resolve().parent
source=Path('/Volumes/Fast/CaraBuild/cara-m1-jit-metal-current-20261125')
ready_path=root/'LATENCY-RENDERER-SOURCE-READY.json'
ready=d.read(ready_path);release=d.read(root/'LATENCY-RENDERER-RELEASE.json')
assert release['released'] is True and release['source_ready_sha256']==d.sha(ready_path)
assert ready['runner_sha256']==d.sha(Path(__file__))
for p,h in ready['harness_hashes'].items():assert d.sha(Path(p))==h
normal_quiet=d.base.quiet;current_case=None;background=[]
def quiet(owned=None):
    normal_quiet(owned)
    for i in range(3 if owned is None else 1):
        if i:time.sleep(3);normal_quiet()
        s=host.snapshot();scored=owned is None or (current_case is not None and
            ((current_case/'guest/frame-2400').is_dir() or
             (current_case/'guest/native-frame-2400.json').is_file()))
        s.update(phase='preflight' if owned is None else 'scored' if scored else 'unscored-startup',case_name=current_case.name if current_case else None)
        background.append(s)
        s['background_allowed_by_user']=True
d.base.quiet=quiet
d.base.ROM=Path(ready['rom']['path']);quiet()
rev,binaries,audits=d.signin(source/'work/m1-jit-dual',source)
assert rev==ready['revision'] and d.sha(d.base.ROM)==ready['rom']['sha256']
identity=d.base.rom_identity();assert list(identity)==ready['rom']['identity']
for k,p in binaries.items():assert d.sha(p)==ready['binaries'][k]['sha256']
for p,h in ready['source_hashes'].items():assert d.sha(source/p)==h
for p,h in ready['gate_hashes'].items():assert d.sha(root/p)==h and d.read(root/p)['passed']
original=d.host_flags
shared=ready['shared_row_gather']
def flags(*a,**kw):
    env=original(*a,**kw)
    if a[0]=='metal':
        env.update(CARA_METAL_GATHER_RESOLVED_ROWS='1',CARA_METAL_GATHER_SHARED_RESOLVED_ROWS=str(int(shared)), CARA_NATIVE_METAL_LATENCY_ACTIVITY='1', CARA_NATIVE_METAL_INHERIT_UI_QOS='0', CARA_NATIVE_PACING_OBSERVATION='0')
        for k in ['INDEX_SNAPSHOT_PROOF','VERIFY_INDEX_SNAPSHOT_PROOF','REUSE_RAW_RESOLVE_PROOF',
                  'REUSE_REJECTED_VERTEX_PROBES','VERTEX_CACHE_ORDERED_EVICTION','REUSE_SAMPLER_BINDINGS',
                  'REUSE_SCAN_STATUS','CACHE_COMPLETED_WRITERS']:env['CARA_METAL_'+k]='0'
    return env
d.host_flags=flags
for sig in (signal.SIGTERM,signal.SIGHUP):signal.signal(sig,d.warm.interrupted)
out=root/'latency-renderer-matched-1';out.mkdir()
report={'passed':False,'revision':rev,'cases':{},'comparisons':{},'scope':ready['scope'],
        'source_ready_sha256':d.sha(ready_path),'shared_row_gather':shared}
data={}
try:
    for name,backend in [('gl-first','gl'),('metal-first','metal'),('metal-final','metal'),('gl-final','gl')]:
        quiet();d.signin(source/'work/m1-jit-dual',source)
        if d.base.rom_identity()!=identity:raise RuntimeError('ROM metadata changed')
        current_case=out/name
        guest,log,case=d.run_case(current_case,binaries[backend],backend,source,native_capture=True)
        report['cases'][name]=case
        try:
            data[name]=d.summarize(guest,log,case,current_case)
            if backend=='metal':
                assert 'HOST_LATENCY_ACTIVITY enabled=1' in log and 'HOST_LATENCY_ACTIVITY ended=1' in log
                assert 'HOST_QOS_INHERIT enabled=0' in log
                assert not (guest/'native-pacing-only.csv').exists()
                assert 'Metal gathered checked resolved rows with original word-copy kernel' in log
                assert ('Metal gathered checked rows using shared copy pipeline' in log)==shared
                q=re.search(r'Native Metal submission window=(\d+) adaptive=(\w+)',log)
                assert q and q[1]=='8' and q[2] in ('true','1')
                assert not re.findall(r'CARA_REGULAR_STAGING runs=',log)
            # Same-renderer frozen selected outputs plus full state/work/file checks.
            ref=ready['references'][backend];folder=Path(ref['folder']);old=d.read(Path(ref['case_file']))
            for key in ref.get('case_key',[]):old=old[key]
            oldguest=folder/'guest';raw=(d.read_ledger(oldguest,backend),
                {f:d.pixels((d.rgb_metal if backend=='metal' else d.rgb_gl)(oldguest,f)[0]) for f in d.CAPTURES})
            c=d.compare(raw,data[name],old,case);report['comparisons'][name+'_vs_retained']=c
            if not c['exact_both_lcds_and_work']:raise RuntimeError('Selected output/state/work/file regression')
            if name!='gl-first':
                c=d.compare(data['gl-first'],data[name],report['cases']['gl-first'],case)
                report['comparisons'][name+'_vs_gl']=c
                if c['state_mismatch_refreshes'] or not c['guest_sdmc_match']:
                    raise RuntimeError('Renderer guest state/work/file mismatch')
            if name=='metal-final':
                c=d.compare(data['metal-first'],data[name],report['cases']['metal-first'],case)
                assert c['exact_both_lcds_and_work'];report['comparisons']['metal_repeat']=c
            if name=='gl-final':assert report['comparisons'][name+'_vs_gl']['exact_both_lcds_and_work']
        except BaseException as e:case.update(passed=False,error=repr(e));raise
        finally:d.write(current_case/'CASE.json',case);d.write(out/'RESULT.json',report)
        print(name,json.dumps(case['stats']),flush=True)
    metrics={}
    for metric in ['mean','p99','max','worst_rolling_120','owner_cpu_ms_per_refresh','process_cpu_ms_per_refresh']:
        gl=[report['cases'][n]['stats'][metric] for n in ['gl-first','gl-final']]
        metal=[report['cases'][n]['stats'][metric] for n in ['metal-first','metal-final']]
        drift=max(abs(gl[1]-gl[0]),abs(metal[1]-metal[0]));margin=min(gl)-max(metal)
        metrics[metric]={'gl':gl,'metal':metal,'max_within_renderer_drift':drift,'metal_margin':margin,'metal_below_both_gl_beyond_drift':margin>drift}
    # Per-PID cumulative CPU deltas over the observed late-scene window.
    daemon_windows={}
    for name,case in report['cases'].items():
        samples=[v for v in background if v.get('case_name')==name and v['phase']=='scored']
        total=0.0;interval=0.0;pid_changes=0
        for a,b in zip(samples,samples[1:]):
            before={x['pid']:x for x in a['services']};after={x['pid']:x for x in b['services']}
            interval+=b['monotonic_s']-a['monotonic_s']
            pid_changes+=len(set(before)^set(after))
            total+=sum(max(0,after[p]['cpu_seconds']-before[p]['cpu_seconds']) for p in set(before)&set(after))
        daemon_windows[name]={'observed_cpu_seconds':total,'observed_wall_seconds':interval,
            'mean_observed_core_equivalents':total/interval if interval>0 else None,
            'sample_count':len(samples),'pid_changes_between_samples':pid_changes,
            'scope':'Capture2400 through clean exit, not exact400-refresh ROI; CPU for process lifetimes outside consecutive observations is unsampled.'}
    report.update(passed=True,metrics=metrics,daemon_windows=daemon_windows,rom_final_sha256=d.sha(d.base.ROM))
    assert report['rom_final_sha256']==ready['rom']['sha256']
except BaseException as e:report.update(passed=False,error=repr(e));raise
finally:report['background_checks']=background;d.write(out/'RESULT.json',report)
