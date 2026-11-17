"""Synthetic post-exit journal and decision checks; no driver or CPP reader run."""
from pathlib import Path
import copy,json,struct,tempfile
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import gl_host_latency_summary as s
D=Path(__file__).resolve().parent

def synthetic():
 # SYNTHETIC capture0 records, not a newly observed process or driver result.
 pid=123;fp='a'*64;data=bytearray(33840);struct.pack_into('<8sIIIIQQQ',data,0,b'CARAHLT1',1,33840,112,64,pid,456,1);data[48:113]=fp.encode()+b'\0';struct.pack_into('<III',data,116,0,125,3);struct.pack_into('<Q',data,128,480000)
 buckets=[]
 for api,cat,calls in[(0,0,72),(1,0,40),(2,2,72),(3,3,40),(4,1,32),(4,0,399348)]:
  hist=[0]*64;hist[6]=calls;ticks=96*calls;maximum=96
  if(api,cat)==(4,1):hist[6]-=1;hist[18]=1;ticks=480000+96*(calls-1);maximum=480000
  b=dict(api=api,category=cat,calls=calls,ticks=ticks,max_ticks=maximum,hist_raw_log2=hist);buckets.append(b);off=432+(api*6+cat)*536;struct.pack_into('<QQQ64Q',data,off,calls,ticks,maximum,*hist)
 total=sum(x['calls']for x in buckets);struct.pack_into('<Q',data,168,2*total);struct.pack_into('<QQQQ',data,400,total,total,1,0)
 latest=dict(publication=2*total,token=total,api=4,pname=0,phase=2,record_flags=0,observer_flags=0,begin_tick=500000,end_tick=500096,timebase_numer=125,timebase_denom=3)
 slow=dict(slow_sequence=1,token=total-1,api=4,pname=0,flags=2,begin_tick=1000,end_tick=481000)
 aggregate=dict(aggregate=True,publication=total,completed_calls=total,slow_returns=1,overwritten_slow=0,flags=0,timebase_numer=125,timebase_denom=3,buckets=buckets)
 return bytes(data),[latest,slow,aggregate],pid,fp

def reject(name,fn):
 try:fn()
 except(ValueError,AssertionError,KeyError):checks.append(name);return
 raise AssertionError('negative accepted '+name)
def run_checks():
 global checks
 checks=[]
 data,rows,pid,fp=synthetic();h=s.header(data,pid,fp);v=s.validate(rows,h);m=s.metrics(v);assert m['first_draw']['calls']==32 and m['first_draw']['max_ns']==20000000;checks.append('SYNTHETIC capture0 nominal/hist/20ms positive NOT runtime')
 with tempfile.TemporaryDirectory(prefix='cara-host-summary-fixture-')as temp:
  j=Path(temp)/'journal.bin';out=Path(temp)/'reader.jsonl';j.write_bytes(data);out.write_text('\n'.join(json.dumps(x)for x in rows)+'\n');assert s.read(j,out,pid,fp)['api_counts']==v['api_counts'];checks.append('SYNTHETIC raw bucket/CPP JSON cross-check positive')
  d=bytearray(data);struct.pack_into('<Q',d,432,73);j.write_bytes(d);reject('raw/CPP bucket mismatch',lambda:s.read(j,out,pid,fp))
 for label,offset,fmt,value in[('schema',8,'I',2),('journal bytes',12,'I',0),('record bytes',16,'I',104),('capacity',20,'I',63),('PID',24,'Q',999),('owner',40,'Q',0),('capture',116,'I',1),('timebase',120,'I',0),('threshold',128,'Q',1),('flags',136,'Q',4),('owner misuse',144,'Q',1),('reentry',152,'Q',1),('metadata unknown',160,'Q',1),('publication',168,'Q',1),('stats publication',400,'Q',1),('ring counter',424,'Q',1)]:
  d=bytearray(data);struct.pack_into('<'+fmt,d,offset,value);reject(label,lambda d=d:s.header(bytes(d),pid,fp))
 reject('wrong FP',lambda:s.header(data,pid,'b'*64));reject('partial journal',lambda:s.header(data[:-1],pid,fp))
 for label,mutate in[
  ('summary censor',lambda q:q[-1].update(flags=8)),('summary call count',lambda q:q[-1].update(completed_calls=1)),('duplicate bucket',lambda q:q[-1]['buckets'].append(copy.deepcopy(q[-1]['buckets'][0]))),('bool counter',lambda q:q[-1]['buckets'][0].update(calls=True)),('hist sum',lambda q:q[-1]['buckets'][0]['hist_raw_log2'].__setitem__(6,0)),('hist bounds',lambda q:q[-1]['buckets'][0].update(ticks=1)),('max bounds',lambda q:q[-1]['buckets'][0].update(max_ticks=1)),('pending',lambda q:q[0].update(phase=1)),('record unknown',lambda q:q[0].update(record_flags=1)),('false link',lambda q:q[1].update(flags=32)),('slow threshold',lambda q:q[1].update(end_tick=1001)),('missing slow',lambda q:q.pop(1)),('unknown row',lambda q:q.append({'unrecognized':1})),('scope nominal mismatch',lambda q:q[-1]['buckets'][0].update(calls=71,ticks=6816,hist_raw_log2=[0]*6+[71]+[0]*57)),
 ]:
  q=copy.deepcopy(rows);mutate(q);reject(label,lambda q=q:s.validate(q,h))
 # Decision fixtures use synthetic scalar metrics only; not measured improvement.
 def result(name,F,maximum,wall=200000000,cpu=90000000):
  return dict(case=name,native_wall_ns=wall,process_lifetime_cpu_ns=cpu,host_metrics={'count_vector':{'4:1':32},'first_draw':{'ns':F,'max_ns':maximum,'calls':32},'timed_prepare_plus_first':{'ns':F+10000000},'ordinary_draw':{'ns':1000000},'all_timed_driver':{'ns':F+11000000}})
 cases=[result('O1',100000000,30000000),result('S1',50000000,22000000,wall=100000000),result('S2',51000000,21000000,wall=101000000),result('O2',101000000,31000000)]
 assert s.decision(cases)['outcome']=='LIMITED_FIRST_OBSERVED_HOST_SPAN_IMPROVEMENT';checks.append('SYNTHETIC conservative decision positive')
 no=[result('O1',20000,1000),result('S1',10000,500,wall=100000000),result('S2',11000,500,wall=101000000),result('O2',21000,1000)];assert s.decision(no)['outcome']=='NO_QUALIFYING_OFF_PAUSE';checks.append('SYNTHETIC no-pause closure')
 shifted=copy.deepcopy(cases);shifted[1]['native_wall_ns']*=4;assert s.decision(shifted)['outcome']=='NO_CONVINCING_BENEFIT_OR_COST_SHIFT';checks.append('SYNTHETIC cost shift closure')
 q=copy.deepcopy(cases);q[1]['host_metrics']['count_vector']={'4:1':31};reject('decision work mismatch',lambda:s.decision(q))
 q=copy.deepcopy(cases);q[1]['native_wall_ns']=float('nan');reject('decision NaN activation',lambda:s.decision(q))
 q=copy.deepcopy(cases);q[1]['host_metrics']['first_draw']['max_ns']=1;reject('decision inconsistent sum/max/count',lambda:s.decision(q))
 q=copy.deepcopy(cases);q[1]['native_wall_ns']=1;reject('decision timed spans exceed native wall',lambda:s.decision(q))
 return checks

class HostSummaryTests(unittest.TestCase):
 def test_synthetic_journal_and_decisions(self):
  self.assertEqual(len(run_checks()),42)

if __name__=='__main__':unittest.main()
