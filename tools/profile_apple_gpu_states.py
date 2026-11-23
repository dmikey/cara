#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded rootless Apple GPU IOReport p-state sampling (instrumented runs only).

No writes to device configuration, privileged calls, installs, or GPU workloads.
Uses a private macOS API: unsupported channels/layouts fail closed or retain raw
states with null derived frequency. System-wide, not process-specific telemetry.
The frequency is an active-residency-weighted interval average, not an instant
clock reading. Coordinate with benchmark owners before sampling.

API/layout reference (independently authored Python implementation):
https://github.com/vladkens/macmon/blob/52eb7799fa815cb512de9e4bf4bf801825365e7d/src/sources.rs
https://github.com/vladkens/macmon/blob/52eb7799fa815cb512de9e4bf4bf801825365e7d/src/metrics.rs#L396
"""
import argparse
import ctypes as C
from datetime import datetime, timezone
import json
import hashlib
import os
import platform
import plistlib
import struct
import subprocess
import sys
import time

SOURCE_REVISION='52eb7799fa815cb512de9e4bf4bf801825365e7d'
POLICY_STATES = frozenset({
    'BSTGPUPH', 'GPU_SW', 'GPU_CLTM', 'GPUDVDH', 'PZRSDNCY',
    'PWRCTRL', 'PMU_RC', 'GPU_PPM', 'AFRSTATE', 'AFRCTRL', 'FENDER',
})
POLICY_COUNTERS = frozenset({
    'Throttle Counter HWPI', 'Throttle Counter DPE', 'Throttle Counter Misc',
    'Throttle Counter IVDM', 'Throttle Counter Total',
})


def frequency_metrics(states, frequencies):
    """Strictly map a known ordered state list, preserving raw counters always."""
    if not states or any(value < 0 for _,value in states):
        return dict(derived_error='missing/negative residency delta')
    active=[(name,value) for name,value in states if name not in ('OFF','IDLE','DOWN')]
    total=sum(value for _,value in states);busy=sum(value for _,value in active)
    result=dict(active_ratio=busy/total if total else None)
    # macmon's GPUPH handler explicitly removes table[0] (OFF), then maps
    # each remaining record to P1 onward. Nonmonotonic/duplicate frequencies
    # are valid: retain the device's record order without sorting/deduplication.
    if not frequencies or frequencies[0]!=0 or any(x<=0 for x in frequencies[1:]):
        return dict(result,derived_error='missing OFF-prefixed frequency table')
    mapping={f'P{i}':freq for i,freq in enumerate(frequencies) if i}
    missing=[name for name in mapping if name not in dict(active)]
    unknown={name:value for name,value in active if name not in mapping}
    result.update(state_frequencies_mhz=mapping,unmapped_state_residencies=unknown,
                  frequency_mapping='GPUPH Pn -> voltage-states9 record n; record0 OFF (macmon reference).')
    if missing or any(unknown.values()):
        return dict(result,derived_error='missing mapped states or active states without a frequency record')
    return dict(result,active_frequency_mhz=sum(value*mapping.get(name,0) for name,value in active)/busy if busy else None)



def frequency_tables():
    result=subprocess.run(['ioreg','-a','-r','-n','pmgr','-d','1'],check=True,capture_output=True)
    entries=plistlib.loads(result.stdout);tables=[]
    def visit(value):
        if isinstance(value,dict):
            raw=value.get('voltage-states9')
            if isinstance(raw,bytes) and raw and len(raw)%8==0:
                records=list(struct.iter_unpack('<II',raw))
                tables.append(dict(node=value.get('IORegistryEntryName'),property='voltage-states9',
                                   raw_frequency_hz=[x[0] for x in records],voltage_raw=[x[1] for x in records],
                                   frequency_mhz=[x[0]/1_000_000 for x in records]))
            for nested in value.values():visit(nested)
        elif isinstance(value,list):
            for nested in value:visit(nested)
    visit(entries)
    return tables


class IOReport:
    def __init__(self, include_policy=False):
        self.cf=C.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        self.io=C.CDLL('/usr/lib/libIOReport.dylib')
        def bind(lib,name,result,args):
            fn=getattr(lib,name);fn.restype=result;fn.argtypes=args;return fn
        P=C.c_void_p;L=C.c_long;U=C.c_uint64
        for name,result,args in (
            ('CFStringCreateWithCString',P,[P,C.c_char_p,C.c_uint32]),
            ('CFStringGetCString',C.c_bool,[P,C.c_char_p,L,C.c_uint32]),
            ('CFDictionaryGetValue',P,[P,P]),('CFDictionaryCreateMutableCopy',P,[P,L,P]),
            ('CFDictionarySetValue',None,[P,P,P]),('CFArrayGetCount',L,[P]),
            ('CFArrayGetValueAtIndex',P,[P,L]),('CFArrayCreateMutable',P,[P,L,P]),
            ('CFArrayAppendValue',None,[P,P]),('CFRelease',None,[P])):
            setattr(self,name,bind(self.cf,name,result,args))
        for name,result,args in (
            ('IOReportCopyAllChannels',P,[U,U]),
            ('IOReportCreateSubscription',P,[P,P,C.POINTER(P),U,P]),
            ('IOReportCreateSamples',P,[P,P,P]),('IOReportCreateSamplesDelta',P,[P,P,P]),
            ('IOReportChannelGetGroup',P,[P]),('IOReportChannelGetSubGroup',P,[P]),
            ('IOReportChannelGetChannelName',P,[P]),('IOReportChannelGetUnitLabel',P,[P]),
            ('IOReportStateGetCount',C.c_int,[P]),('IOReportStateGetNameForIndex',P,[P,C.c_int]),
            ('IOReportStateGetResidency',C.c_int64,[P,C.c_int])):
            setattr(self,name,bind(self.io,name,result,args))
        if include_policy:
            self.IOReportSimpleGetIntegerValue = bind(self.io, 'IOReportSimpleGetIntegerValue',
                                                       C.c_int64, [P, C.c_int])
        self.key=self.CFStringCreateWithCString(None,b'IOReportChannels',0x08000100)
        self.source=self.IOReportCopyAllChannels(0,0)
        if not self.source:raise RuntimeError('IOReportCopyAllChannels unavailable')
        self.selected_channels=self.CFDictionaryCreateMutableCopy(None,0,self.source)
        # CFArrayCallBacks: CFIndex version + four function pointers.
        class Callbacks(C.Structure):
            _fields_=[('version',L),('retain',P),('release',P),('description',P),('equal',P)]
        callbacks=Callbacks.in_dll(self.cf,'kCFTypeArrayCallBacks')
        self.array=self.CFArrayCreateMutable(None,0,C.byref(callbacks))
        for channel in self.channels(self.source):
            group = self.string(self.IOReportChannelGetGroup(channel))
            if group != 'GPU Stats':
                continue
            subgroup = self.string(self.IOReportChannelGetSubGroup(channel))
            name = self.string(self.IOReportChannelGetChannelName(channel)) if include_policy else ''
            policy = include_policy and (name in POLICY_STATES or
                        (subgroup == 'GPU Throttler Counters' and name in POLICY_COUNTERS))
            if subgroup == 'GPU Performance States' or policy:
                self.CFArrayAppendValue(self.array,channel)
        if not self.CFArrayGetCount(self.array):raise RuntimeError('No GPU Performance States channels available')
        self.CFDictionarySetValue(self.selected_channels,self.key,self.array)
        actual=P()
        self.subscription=self.IOReportCreateSubscription(None,self.selected_channels,C.byref(actual),0,None)
        self.actual=actual.value
        if not self.subscription or not self.actual:raise RuntimeError('Rootless GPU IOReport subscription unavailable')
        if not list(self.channels(self.actual)):raise RuntimeError('GPU channels omitted from actual subscription')

    def string(self,ref):
        if not ref:return ''
        text=C.create_string_buffer(4096)
        if not self.CFStringGetCString(ref,text,len(text),0x08000100):raise RuntimeError('IOReport channel name too long')
        return text.value.decode('utf-8')

    def channels(self,data):
        array=self.CFDictionaryGetValue(data,self.key)
        if array:
            for i in range(self.CFArrayGetCount(array)):yield self.CFArrayGetValueAtIndex(array,i)

    def sample(self):
        result=self.IOReportCreateSamples(self.subscription,self.actual,None)
        if not result:raise RuntimeError('IOReport sample unavailable')
        return result

    def delta(self,before,after,frequencies):
        delta=self.IOReportCreateSamplesDelta(before,after,None)
        if not delta:raise RuntimeError('IOReport delta unavailable')
        try:
            output=[]
            for item in self.channels(delta):
                subgroup = self.string(self.IOReportChannelGetSubGroup(item))
                name = self.string(self.IOReportChannelGetChannelName(item))
                row = dict(group=self.string(self.IOReportChannelGetGroup(item)), subgroup=subgroup,
                           channel=name, unit=self.string(self.IOReportChannelGetUnitLabel(item)))
                count = self.IOReportStateGetCount(item)
                if count >= 0:
                    states = [(self.string(self.IOReportStateGetNameForIndex(item, i)),
                               self.IOReportStateGetResidency(item, i)) for i in range(count)]
                    row['states'] = states
                    if subgroup == 'GPU Performance States' and name == 'GPUPH':
                        row.update(frequency_metrics(states, frequencies))
                elif subgroup == 'GPU Throttler Counters' and name in POLICY_COUNTERS:
                    row['raw_delta'] = self.IOReportSimpleGetIntegerValue(item, 0)
                output.append(row)
            return output
        finally:self.CFRelease(delta)

    def close(self):
        for name in ('actual','subscription','array','selected_channels','source','key'):
            ref=getattr(self,name,None)
            if ref:self.CFRelease(ref);setattr(self,name,None)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--samples',type=int,default=10)
    parser.add_argument('--interval-ms',type=int,default=1000)
    parser.add_argument('--policy-states', action='store_true',
                        help='Record raw GPU controller and throttling channels exposed by this Mac')
    args=parser.parse_args()
    if not 1<=args.samples<=7200 or not 100<=args.interval_ms<=60000:parser.error('Require samples1..7200, interval-ms100..60000')
    if sys.platform!='darwin':parser.error('Requires macOS')
    tables=frequency_tables()
    unique={tuple(x['frequency_mhz']) for x in tables}
    frequencies=list(next(iter(unique))) if len(unique)==1 else []
    print(json.dumps(dict(type='metadata',platform=platform.platform(),pid=os.getpid(),
                          sampler_sha256=hashlib.sha256(open(__file__,'rb').read()).hexdigest(),
                          instrumented=True,system_wide=True,api='private macOS IOReport',
                          policy_states=args.policy_states,
                          upstream_reference_revision=SOURCE_REVISION,frequency_tables=tables,
                          scope='Active-residency-weighted interval average; not instantaneous or per-process frequency.')) ,flush=True)
    report=IOReport(include_policy=args.policy_states);previous=None
    try:
        previous=report.sample();start=time.monotonic();wall=datetime.now(timezone.utc).isoformat()
        for i in range(args.samples):
            target=start+args.interval_ms/1000
            while time.monotonic()<target:time.sleep(max(0,min(1,target-time.monotonic())))
            current=report.sample();end=time.monotonic()
            try:data=report.delta(previous,current,frequencies)
            except BaseException:report.CFRelease(current);raise
            report.CFRelease(previous);previous=current
            print(json.dumps(dict(type='sample',index=i,start_utc=wall,end_utc=datetime.now(timezone.utc).isoformat(),
                                  interval_seconds=end-start,channels=data)),flush=True)
            start=end;wall=datetime.now(timezone.utc).isoformat()
    finally:
        if previous:report.CFRelease(previous)
        report.close()


if __name__=='__main__':main()
