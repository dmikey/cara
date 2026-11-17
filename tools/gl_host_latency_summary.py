"""Post-exit validation of immutable host journals and existing CPP-reader JSON."""
from pathlib import Path
import json,struct,math
MAX=2**64-1

def require(ok,why):
 if not ok:raise ValueError(why)
def uint(v):return type(v)is int and 0<=v<=MAX
def header(data,pid,fp,capture=0):
 require(type(data)is bytes and len(data)==33840,'journal bytes')
 require(type(pid)is int and pid>0 and type(fp)is str and len(fp)==64 and all(x in'0123456789abcdef'for x in fp),'expected identity')
 require(type(capture)is int and capture in[0,1],'capture expectation')
 u32=lambda n:struct.unpack_from('<I',data,n)[0];u64=lambda n:struct.unpack_from('<Q',data,n)[0]
 require(data[:8]==b'CARAHLT1' and [u32(n)for n in[8,12,16,20]]==[1,33840,112,64],'journal ABI')
 require(u64(24)==pid and u64(32)>0 and u64(40)==1,'owner/PID')
 require(data[48:113]==fp.encode()+b'\0' and u32(116)==capture,'FP/capture')
 n,d=u32(120),u32(124);require(n>0 and d>0,'timebase');threshold=(20000000*d+n-1)//n
 require(u64(128)==threshold and all(u64(o)==0 for o in[136,144,152,160]),'threshold/flags/owner/reentry/metadata censor')
 # Offsets follow the byte-bound schema: latest[2] starts176,2*112B;
 # stats publication400, completed408, slow416, overwritten424, buckets432.
 h=dict(pid=pid,fp=fp,capture=capture,numer=n,denom=d,threshold=threshold,committed=u64(168),stats_publication=u64(400),completed_calls=u64(408),slow_returns=u64(416),overwritten_slow=u64(424))
 require(h['committed']>0 and h['stats_publication']>0 and h['completed_calls']>0,'no completed publication')
 require(h['committed']==2*h['completed_calls'] and h['stats_publication']==h['completed_calls'],'publication closure')
 require(h['slow_returns']<=h['completed_calls'] and h['overwritten_slow']==max(0,h['slow_returns']-64),'slow ring counters')
 return h

def validate(rows,h,nominal=True):
 require(type(rows)is list and rows,'reader rows')
 summaries=[r for r in rows if r.get('aggregate')is True];latest=[r for r in rows if 'record_flags'in r];slow=[r for r in rows if 'slow_sequence'in r]
 require(len(summaries)==len(latest)==1 and len(rows)==2+len(slow),'reader row inventory')
 a,r=summaries[0],latest[0]
 for key in['publication','completed_calls','slow_returns','overwritten_slow','flags','timebase_numer','timebase_denom']:require(uint(a.get(key)),'summary type '+key)
 require(a['publication']==h['stats_publication'] and all(a[k]==h[k]for k in['completed_calls','slow_returns','overwritten_slow']),'header/summary counters')
 require(a['flags']==0 and a['timebase_numer']==h['numer']and a['timebase_denom']==h['denom'],'summary censor/timebase')
 require(uint(r.get('publication'))and r['publication']==h['committed']and uint(r.get('token'))and r['token']==h['completed_calls'],'latest publication/token')
 for key in['phase','observer_flags','timebase_numer','timebase_denom']:require(uint(r.get(key)),'latest type '+key)
 require(r.get('phase')==2 and r.get('observer_flags')==0 and r.get('timebase_numer')==h['numer']and r.get('timebase_denom')==h['denom'],'latest complete/observer')
 def record(v,flagkey):
  for k in['api','pname','begin_tick','end_tick',flagkey]:require(uint(v.get(k)),'record type '+k)
  require(v['api']<8 and v['begin_tick']>0 and v['end_tick']>=v['begin_tick'],'record bounds')
  require(v[flagkey]<64 and not(v[flagkey]&(1|16|32)),'unknown/unobserved/false-link record')
 record(r,'record_flags')
 if r['api']in[2,3]and r['pname']in[0x8b81,0x8b82]:require(r['record_flags']&4 and type(r.get('status'))is int and r['status']==1,'latest compile/link status')
 sequences=set();tokens=set()
 for v in slow:
  record(v,'flags');seq=v.get('slow_sequence');token=v.get('token');require(uint(seq)and uint(token)and seq>0 and token>0 and token<=h['completed_calls']and seq not in sequences and token not in tokens,'slow identity')
  require(v['end_tick']-v['begin_tick']>=h['threshold'],'slow threshold');sequences.add(seq);tokens.add(token)
 require(sequences==set(range(max(1,h['slow_returns']-63),h['slow_returns']+1)),'slow ring coverage')
 buckets={};require(type(a.get('buckets'))is list,'buckets type')
 for b in a['buckets']:
  for key in['api','category','calls','ticks','max_ticks']:require(uint(b.get(key)),'bucket type '+key)
  api,cat=b['api'],b['category'];require(api<8 and cat<6 and (api,cat)not in buckets and b['calls']>0,'bucket key/calls')
  require((api in[0,1]and cat==0)or(api in[4,5,6]and cat in[0,1])or(api in[2,3]and cat in[0,2,3,4,5])or api==7,'API/category')
  require(cat!=2 or api==2,'compile category');require(cat!=3 or api==3,'link category')
  hist=b.get('hist_raw_log2');require(type(hist)is list and len(hist)==64 and all(uint(x)for x in hist)and sum(hist)==b['calls'],'histogram count/type')
  occupied=[i for i,v in enumerate(hist)if v];require(occupied,'empty occupied histogram')
  lower=sum(v*(0 if i==0 else 2**i)for i,v in enumerate(hist));upper=sum(v*(1 if i==0 else 2**(i+1)-1)for i,v in enumerate(hist))
  m=b['max_ticks'];require(lower<=b['ticks']<=upper and m<=b['ticks']<=m*b['calls'],'tick/hist/max bounds')
  top=occupied[-1];require((0 if top==0 else 2**top)<=m<=(1 if top==0 else 2**(top+1)-1),'max occupied bin')
  # Explicitly compare the summary bucket with the immutable raw journal later.
  buckets[(api,cat)]=b
 require(sum(b['calls']for b in buckets.values())==h['completed_calls'],'all calls sum')
 slow_lower=slow_upper=0
 for b in buckets.values():
  if b['max_ticks']<h['threshold']:continue
  lo=sum(v for i,v in enumerate(b['hist_raw_log2'])if(0 if i==0 else 2**i)>=h['threshold'])
  hi=sum(v for i,v in enumerate(b['hist_raw_log2'])if(1 if i==0 else 2**(i+1)-1)>=h['threshold'])
  slow_lower+=max(1,lo);slow_upper+=hi
 require(slow_lower<=h['slow_returns']<=slow_upper,'slow histogram threshold bounds')
 require(h['capture']!=0 or not any(k[0]==7 for k in buckets),'capture observer work')
 api_counts=[sum(v['calls']for k,v in buckets.items()if k[0]==api)for api in range(8)]
 first=sum(b['calls']for k,b in buckets.items()if k[0]in[4,5,6]and k[1]==1)
 require(first>0,'no first-observed draw')
 if nominal:
  require(api_counts[0]==72 and api_counts[1]==40 and sum(api_counts[4:7])==399380,'nominal compile/link/draw coverage')
  require(buckets.get((2,2),{}).get('calls')==72 and buckets.get((3,3),{}).get('calls')==40,'compile/link status query coverage')
 return dict(header=h,buckets=buckets,api_counts=api_counts,first_draw_calls=first,slow_identity_censored=h['overwritten_slow']>0)

def read(journal,dump,pid,fp,capture=0,nominal=True):
 data=Path(journal).read_bytes();h=header(data,pid,fp,capture);rows=[json.loads(x)for x in Path(dump).read_text().splitlines()if x.strip()];v=validate(rows,h,nominal)
 for index in range(48):
  off=432+index*536;calls,ticks,maximum=struct.unpack_from('<QQQ',data,off);hist=list(struct.unpack_from('<64Q',data,off+24));b=v['buckets'].get((index//6,index%6))
  require((b is None and calls==ticks==maximum==0 and not any(hist))or(b is not None and [calls,ticks,maximum,hist]==[b['calls'],b['ticks'],b['max_ticks'],b['hist_raw_log2']]),'raw/CPP bucket disagreement')
 return v

def metrics(v):
 h=v['header'];b=v['buckets'];n,d=h['numer'],h['denom']
 def group(predicate):
  selected=[x for k,x in b.items()if predicate(*k)];ticks=sum(x['ticks']for x in selected);maximum=max([x['max_ticks']for x in selected]or[0]);hist=[sum(x['hist_raw_log2'][i]for x in selected)for i in range(64)]
  return dict(calls=sum(x['calls']for x in selected),ticks=ticks,ns=ticks*n//d,max_ticks=maximum,max_ns=maximum*n//d,hist_raw_log2=hist)
 return dict(all_timed_driver=group(lambda a,c:a<7),first_draw=group(lambda a,c:a in[4,5,6]and c==1),compile=group(lambda a,c:a==0),link=group(lambda a,c:a==1),shader_query=group(lambda a,c:a==2),program_query=group(lambda a,c:a==3),ordinary_draw=group(lambda a,c:a in[4,5,6]and c!=1),timed_prepare_plus_first=group(lambda a,c:a in[0,1,2,3]or(a in[4,5,6]and c==1)),api_counts=v['api_counts'],count_vector={str(a)+':'+str(c):x['calls']for(a,c),x in b.items()},timebase=dict(numer=n,denom=d),slow_identity_censored=v['slow_identity_censored'],rounding='ns floor rational conversion; raw ticks/timebase retained',scope='Worker HOST API spans including entry publication; not GPU time/internal compiler-only cost')

def decision(results):
 require([x['case']for x in results]==['O1','S1','S2','O2'],'schedule')
 vectors=[x['host_metrics']['count_vector']for x in results]
 for r,v in zip(results,vectors):
  require(type(v)is dict and v and all(type(k)is str and uint(n)and n>0 for k,n in v.items()),'decision count types')
  fd=r['host_metrics']['first_draw']
  require(all(uint(fd.get(k))for k in['ns','max_ns','calls'])and fd['calls']>0,'first decision metric types')
  require(fd['max_ns']<=fd['ns']<=fd['calls']*(fd['max_ns']+1),'first metric count/max bounds')
  for key in['timed_prepare_plus_first','ordinary_draw','all_timed_driver']:require(uint(r['host_metrics'][key].get('ns')),'complement metric types')
  for key in['native_wall_ns','process_lifetime_cpu_ns']:require(uint(r.get(key))and r[key]>0,'activation metric types')
  whole=r['host_metrics']['all_timed_driver']['ns'];parts=r['host_metrics']['timed_prepare_plus_first']['ns']+r['host_metrics']['ordinary_draw']['ns']
  require(parts<=whole<=parts+1 and whole<=r['native_wall_ns'],'disjoint host spans/native wall consistency')
 require(all(v==vectors[0]for v in vectors),'cross-case API/category work')
 off=[results[0],results[3]];on=results[1:3]
 def vals(rows,key):
  value=[r['host_metrics'][key]['ns']for r in rows];require(all(uint(x)for x in value),'metric uint');return value
 f0,f1=vals(off,'first_draw'),vals(on,'first_draw');c0,c1=vals(off,'timed_prepare_plus_first'),vals(on,'timed_prepare_plus_first');drift=abs(f0[1]-f0[0]);margin=min(f0)-max(f1)
 for x in results:require(uint(x['host_metrics']['first_draw'].get('max_ns')),'first max type')
 qualifying=any(x['host_metrics']['first_draw']['max_ns']>=20000000 for x in off)
 regressions=[]
 for key in['native_wall_ns','process_lifetime_cpu_ns']:
  a=[r[key]for r in off];b=[r[key]for r in on];require(all(uint(x)and x>0 for x in a+b),'activation metric '+key)
  if sum(b)>sum(a)+2*abs(a[1]-a[0]):regressions.append(key)
 for key in['timed_prepare_plus_first','ordinary_draw','all_timed_driver']:
  a,b=vals(off,key),vals(on,key)
  if sum(b)>sum(a)+2*abs(a[1]-a[0]):regressions.append(key)
 activation_support=False
 for key in['native_wall_ns','process_lifetime_cpu_ns']:
  a,b=[r[key]for r in off],[r[key]for r in on]
  activation_support|=min(a)-max(b)>abs(a[1]-a[0])
 positive=qualifying and margin>drift and not regressions and sum(c1)<sum(c0)and activation_support
 outcome='LIMITED_FIRST_OBSERVED_HOST_SPAN_IMPROVEMENT'if positive else'NO_QUALIFYING_OFF_PAUSE'if not qualifying else'NO_CONVINCING_BENEFIT_OR_COST_SHIFT'
 return dict(outcome=outcome,qualifying_OFF_pause=qualifying,OFF_first_ns=f0,ON_first_ns=f1,OFF_drift_ns=drift,between_controls_margin_ns=margin,activation_regressions=regressions,activation_support_beyond_OFF_drift=activation_support,promoted=False,scope='One driver-cache-warmth-UNKNOWN checkpoint, no universal/GPU/compiler-only/freeze-fix inference; no repeat/variants')
