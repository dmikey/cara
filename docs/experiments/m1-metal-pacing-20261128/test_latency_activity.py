import sys,json,signal,csv,re
from pathlib import Path
sys.dont_write_bytecode=True
sys.path[:0]=[str(Path(__file__).resolve().parent),'/Users/derekanderson/Documents/Codex/2026-11-02/can-you-get-the-latest-citra-3/outputs/cara-m1-jit-metal-current-20261125']
import replay_dual as d
import host_load_observer as host
root=Path(__file__).resolve().parent;source=Path('/Volumes/Fast/CaraBuild/cara-m1-jit-metal-current-20261125');out=root/'latency-activity-pair-1'
d.base.ROM=Path(d.read(root/'local-rom-1/RECEIPT.json')['destination']);d.base.quiet();revision,files,audits=d.signin(source/'work/m1-jit-dual',source)
gate=d.read(root/'latency-activity-build-1/RESULT.json');assert gate['passed'] and gate['revision']==revision
assert d.read(root/'ui-qos-matched-1/RESULT.json')['passed']
ready=d.read(root/'LATENCY-ACTIVITY-SOURCE-READY.json');assert ready['revision']==revision and d.sha(Path(__file__))==ready['runner_sha256']
for p,h in ready['source_hashes'].items():assert d.sha(source/p)==h
for p,h in ready['harness_hashes'].items():assert d.sha(Path(p))==h
assert d.sha(files['metal'])==ready['binary_sha256'] and list(d.base.rom_identity())==ready['rom_identity']
out.mkdir();original=d.host_flags;active=False

def flags(*a,**kw):
 env=original(*a,**kw);env.update(d.read(root/'PRIVATE-PERFORMANCE-PRESET.json')['metal_env'])
 env.update(CARA_NATIVE_METAL_INHERIT_UI_QOS='0',CARA_NATIVE_METAL_LATENCY_ACTIVITY=str(int(active)),CARA_NATIVE_PACING_OBSERVATION='1')
 for k in ['INDEX_SNAPSHOT_PROOF','VERIFY_INDEX_SNAPSHOT_PROOF','REUSE_RAW_RESOLVE_PROOF','REUSE_SAMPLER_BINDINGS','VERTEX_CACHE_ORDERED_EVICTION','REUSE_REJECTED_VERTEX_PROBES','REUSE_SCAN_STATUS','CACHE_COMPLETED_WRITERS']:env['CARA_METAL_'+k]='0'
 return env
d.host_flags=flags
for sig in (signal.SIGTERM,signal.SIGHUP):signal.signal(sig,d.warm.interrupted)
report={'passed':False,'revision':revision,'cases':{},'scope':ready['scope'],'source_ready_sha256':d.sha(root/'LATENCY-ACTIVITY-SOURCE-READY.json'),'host_before':host.snapshot()};data={}
try:
 for name,active in [('off',False),('on',True)]:
  d.base.quiet();d.signin(source/'work/m1-jit-dual',source)
  guest,log,case=d.run_case(out/name,files['metal'],'metal',source,native_capture=True);report['cases'][name]=case
  try:
   data[name]=d.summarize(guest,log,case,out/name)
   assert ('HOST_LATENCY_ACTIVITY enabled=1' in log)==active and ('HOST_LATENCY_ACTIVITY ended=1' in log)==active
   assert 'HOST_QOS_INHERIT enabled=0' in log
   case['qos']=re.findall(r'HOST_QOS thread=(\w+) effective=(\d+) requested=(\d+) relative=(-?\d+)',log)
   rows=list(csv.DictReader((guest/'native-pacing-only.csv').open()));assert len(rows)==3000 and [int(r['ledger_refresh']) for r in rows]==list(range(3000))
   sleeps={int(r['ledger_refresh']):int(r['saved_sleep_ns']) for r in rows};assert all(x>=0 for x in sleeps.values());roi=[sleeps[f]/1e6 for f in range(2401,2801)]
   case['pacing']={'max_saved_sleep_ms':max(roi),'total_saved_sleep_ms':sum(roi),'sleeps_above_requested25ms_bound':sum(x>25 for x in roi),'target_sleep_ms':{str(f):sleeps[f]/1e6 for f in [2526,2706]},'scope':'Actual saved limiter sleep mappedF-1; max25ms requested bound from unmodified cap100 normal limiter. Includes host scheduling delay inside sleep.'}
   ref=root/'ui-qos-matched-1/metal-off-1';old=d.read(ref/'CASE.json');og=ref/'guest';olddata=(d.read_ledger(og,'metal'),{f:d.pixels(d.rgb_metal(og,f)[0]) for f in d.CAPTURES});comp=d.compare(olddata,data[name],old,case);case['retained_comparison']=comp;assert comp['exact_both_lcds_and_work']
   if active:report['comparison']=d.compare(data['off'],data['on'],report['cases']['off'],case);assert report['comparison']['exact_both_lcds_and_work']
  except BaseException as e:case.update(passed=False,error=repr(e));raise
  finally:d.write(out/name/'CASE.json',case);d.write(out/'RESULT.json',report)
  print(name,json.dumps({'stats':case['stats'],'pacing':case['pacing']}),flush=True)
 report.update(passed=True,host_after=host.snapshot())
except BaseException as e:report['error']=repr(e);raise
finally:d.write(out/'RESULT.json',report)
