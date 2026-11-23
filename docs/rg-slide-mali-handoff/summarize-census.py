from collections import Counter, defaultdict
from pathlib import Path
import re
import sys

phase = ''
records = defaultdict(list)
emulation = {}
for line in Path(sys.argv[1]).read_text().splitlines():
    if match := re.search(r'CaraBudget:\s+(boot|title-end|title|profile)-([\w-]+)$', line):
        marker, run = match.groups()
        phase = (run, marker)
    elif match := re.search(r'\s(\d+)\s+(\d+) I CaraPerf: window', line):
        pid, tid = match.groups()
        emulation[pid] = tid
    elif phase and (match := re.search(r'\s(\d+)\s+(\d+) I CaraShader:\s+(\w+) (.*)$', line)):
        pid, tid, action, fields = match.groups()
        records[phase].append((pid, tid, action, dict(re.findall(r'(\w+)=([^ ]*)', fields))))

fragment_stages = {'fragment', 'frag'}
for phase, rows in records.items():
    keys = {r['key']: r['source'] for _, _, a, r in rows if a == 'fragment_key'}
    lit = {r['source'] for _, _, a, r in rows if a == 'fragment_key' and r['lit'] == '1'}
    print(f'{"/".join(phase)}: {len(keys)} new PICA keys, {len(set(keys.values()))} distinct sources, {len(lit)} lit')
    for is_emulation in (True, False):
        scope = [(a, r) for pid, tid, a, r in rows if (emulation.get(pid) == tid) == is_emulation]
        if not scope:
            continue
        sources = [r['source'] for a, r in scope if a == 'compile' and r['stage'] in fragment_stages]
        print('  ', 'emulation' if is_emulation else 'other threads',
              len(sources), 'fragment compiles', len(set(sources)), 'distinct sources')
        for action in ('compile', 'link', 'binary', 'first_draw'):
            times = [int(r['us']) for a, r in scope if a == action]
            if times:
                print(f'    {action}: {len(times)} calls, {sum(times)/1000:.2f} ms total, {max(times)/1000:.2f} ms worst')
        if duplicates := [(source, n) for source, n in Counter(sources).most_common() if n > 1]:
            print('    most repeated fragment sources:', duplicates[:5])
