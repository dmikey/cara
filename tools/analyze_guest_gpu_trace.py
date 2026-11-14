#!/usr/bin/env python3
"""Summarize observed guest GPU volume and dependencies without hardware-latency claims."""
import argparse
from collections import Counter
import csv
import io
import json
from pathlib import Path

FIELDS = ('sequence', 'frame', 'submission', 'timer0_ticks', 'current_ticks', 'global_ticks',
          'kind', 'thread', 'object', 'core', 'a', 'b', 'c', 'd')
KINDS = ('VBlank', 'GXBegin', 'GXEnd', 'IRQRequest', 'Relay', 'EventSignal', 'EventAcquire',
         'EventClear', 'WaitReady', 'WaitPoll', 'WaitBlock', 'WaitObject', 'Wake', 'Run',
         'Swap', 'Binding', 'PicaIRQ', 'Draw', 'DrawState', 'DrawInput', 'HIDUpdate', 'SleepRequest')


def read_trace(text):
    metadata, lines = {}, []
    version = False
    for line in text.splitlines():
        if line.startswith('#'):
            comment = line[1:].strip()
            if comment.startswith('cara_guest_gpu_trace_v1,'):
                version = True
            if comment.startswith(('cara_guest_gpu_trace_v1,', 'foreign_events_dropped=')):
                for item in comment.split(','):
                    if '=' in item:
                        key, value = item.split('=', 1)
                        if key in metadata:
                            raise ValueError(f'duplicate metadata {key}')
                        try:
                            metadata[key] = int(value)
                        except ValueError as error:
                            raise ValueError(f'invalid metadata {key}') from error
        elif line.strip():
            lines.append(line)
    if not version:
        raise ValueError('missing supported trace version')
    reader = csv.DictReader(io.StringIO('\n'.join(lines)))
    if tuple(reader.fieldnames or ()) != FIELDS:
        raise ValueError('unexpected CSV columns')
    rows = []
    for index, row in enumerate(reader):
        try:
            parsed = {key: int(row[key]) for key in FIELDS}
        except (ValueError, TypeError, KeyError) as error:
            raise ValueError(f'invalid CSV record {index}') from error
        if None in row or any(value < 0 for value in parsed.values()):
            raise ValueError(f'extra fields or negative values at record {index}')
        rows.append(parsed)
    return metadata, rows


def validate(metadata, rows, allow_partial):
    required = ('start_frame', 'duration_vblanks', 'truncated', 'complete', 'records',
                'foreign_events_dropped')
    if any(key not in metadata for key in required):
        raise ValueError('missing capture metadata')
    if any(metadata[key] < 0 for key in required):
        raise ValueError('negative capture metadata')
    if not 1 <= metadata['duration_vblanks'] <= 16 or metadata['records'] != len(rows) or len(rows) > 32768:
        raise ValueError('duration, record count or capacity mismatch')
    if metadata['complete'] not in (0, 1) or metadata['truncated'] not in (0, 1):
        raise ValueError('invalid completeness flag')
    problems = []
    if not metadata['complete']:
        problems.append('capture ended before the window completed')
    if metadata['truncated']:
        problems.append('record capacity exhausted; final interval and chains may be cut')
    if metadata['foreign_events_dropped']:
        problems.append('foreign host-thread observations were dropped')
    expected = list(range(metadata['start_frame'], metadata['start_frame'] + metadata['duration_vblanks']))
    boundaries, last_core = [], {}
    last_timer0 = last_global = -1
    for index, row in enumerate(rows):
        if row['sequence'] != index or not 0 <= row['kind'] < len(KINDS):
            raise ValueError(f'invalid sequence or kind at record {index}')
        if row['core'] not in range(4) or row['frame'] not in expected:
            raise ValueError(f'invalid domain or frame at record {index}')
        if row['timer0_ticks'] < last_timer0 or row['global_ticks'] < last_global:
            raise ValueError(f'timer0/global domain regressed at record {index}')
        if row['current_ticks'] < last_core.get(row['core'], -1):
            raise ValueError(f'current timer domain regressed at record {index}')
        if row['core'] == 0 and row['current_ticks'] != row['timer0_ticks']:
            raise ValueError(f'timer0/current disagreement at record {index}')
        last_core[row['core']] = row['current_ticks']
        last_timer0, last_global = row['timer0_ticks'], row['global_ticks']
        if row['kind'] == 0:
            boundaries.append(row['frame'])
        if not boundaries or row['frame'] != boundaries[-1]:
            raise ValueError(f'missing or inconsistent VBlank boundary at record {index}')
    if boundaries != sorted(set(boundaries)):
        raise ValueError('VBlank boundaries are duplicated or unordered')
    if not problems and boundaries != expected:
        raise ValueError('complete capture does not contain all requested VBlank intervals')
    if problems and not allow_partial:
        raise ValueError('; '.join(problems) + '; use --allow-partial for observations only')
    return problems, boundaries, sorted(last_core)


def draw_summary(metadata, rows, example_limit):
    partitions, groups = {}, {}
    by_frame = Counter()
    total_vertices = 0
    triplets = 0
    for index, row in enumerate(rows):
        if row['kind'] != 17:
            if row['kind'] in (18, 19) and (index == 0 or rows[index - 1]['kind'] != row['kind'] - 1):
                raise ValueError('orphan draw state/input record')
            continue
        if index + 2 >= len(rows):
            raise ValueError('incomplete draw triplet')
        state, inputs = rows[index + 1:index + 3]
        if ([state['kind'], inputs['kind']] != [18, 19]
                or any(item['frame'] != row['frame'] or item['submission'] != row['submission']
                       for item in (state, inputs))):
            raise ValueError('non-adjacent or inconsistent draw triplet')
        triplets += 1
        by_frame[row['frame']] += 1
        vertices = row['b'] & 0xffffffff
        total_vertices += vertices
        color, depth = row['a'] >> 32, row['a'] & 0xffffffff
        dims = state['a']
        key = (color, depth, dims & 65535, (dims >> 16) & 65535,
               (dims >> 32) & 15, (dims >> 36) & 15)
        group = partitions.setdefault(key, dict(color_address=color, depth_address=depth,
            width=key[2], height=key[3], color_format=key[4], depth_format=key[5],
            draws=0, trigger_vertices=0, indexed_draws=0, frames=set()))
        group['draws'] += 1
        group['trigger_vertices'] += vertices
        group['indexed_draws'] += bool((row['b'] >> 32) & 1)
        group['frames'].add(row['frame'])
        # These are equality groups of captured fields, never duplicate-render claims.
        signature = tuple(row[name] for name in ('a', 'b', 'c', 'd')) + tuple(
            item[name] for item in (state, inputs) for name in ('a', 'b', 'c', 'd'))
        detail = groups.setdefault(signature, dict(count=0, first_sequence=row['sequence'],
            color_address=color, depth_address=depth, vertices=vertices,
            indexed=bool((row['b'] >> 32) & 1), topology=(row['b'] >> 33) & 3,
            use_gs=(row['b'] >> 35) & 3, variable_primitive=bool((row['b'] >> 37) & 1),
            vs_generation=row['c'], gs_generation=row['d'], width=key[2], height=key[3],
            color_format=key[4], depth_format=key[5], color_write_mask=(dims >> 40) & 15,
            depth_stencil_write_mask=(dims >> 44) & 3, cull_mode=(dims >> 48) & 3,
            depth_color_mask_raw=state['b'] & 0xffffffff, stencil_test_raw=state['b'] >> 32,
            vs_main_offset=state['c'] & 0xffffffff, gs_main_offset=state['c'] >> 32,
            blend_mode_raw=state['d'] & 0xffffffff, alpha_blend_raw=state['d'] >> 32,
            vertex_base=inputs['a'] >> 32, index_raw=inputs['a'] & 0xffffffff,
            vertex_offset=inputs['b'] & 0xffffffff, texture_config_raw=inputs['b'] >> 32,
            texture_addresses=[inputs['c'] >> 32, inputs['c'] & 0xffffffff, inputs['d'] & 0xffffffff],
            texture_formats_mode_raw=inputs['d'] >> 32))
        detail['count'] += 1
    for partition in partitions.values():
        partition['frames'] = sorted(partition['frames'])
    return dict(enabled=bool(metadata.get('draws_enabled', 0)), captured_draws=triplets,
                trigger_vertices=total_vertices,
                per_frame=[dict(frame=frame, draws=count) for frame, count in sorted(by_frame.items())],
                framebuffer_partitions=sorted(partitions.values(), key=lambda item: -item['draws']),
                captured_state_groups=len(groups),
                repeated_captured_state_groups=sum(item['count'] > 1 for item in groups.values()),
                largest_captured_state_groups=sorted(groups.values(), key=lambda item: -item['count'])[:example_limit],
                limitation='Equal captured fields are candidates for inspection, never duplicate proof. Uniforms, viewport/scissor, vertex/texture contents and content identity are not captured. Shader generations are capture-local observed changes, not cross-run identities.')


def analyze(metadata, rows, allow_partial=False, example_limit=20):
    problems, boundaries, domains = validate(metadata, rows, allow_partial)
    draws = draw_summary(metadata, rows, example_limit)
    intervals = {frame: dict(frame=frame, gx_commands=0, command_lists=0, draws=0,
                            gpu_vertices=0, counters_available=True, p3d_requests=0,
                            ppf_requests=0, top_swaps=0) for frame in boundaries}
    begins, requests, pending_signals, visible_signals, acquisitions, activities = ({} for _ in range(6))
    commands, irq_pairs, chains, top_swaps = [], [], [], []
    wait_counts, relay_outcomes = Counter(), Counter()
    active, unmatched_ends = None, 0
    blocked, ready = {}, {}
    pica_writes = Counter()
    for row in rows:
        kind = KINDS[row['kind']]
        seq, frame, sid = row['sequence'], row['frame'], row['submission']
        interval = intervals[frame]
        if kind == 'GXBegin':
            if sid in begins or active is not None:
                raise ValueError('duplicate or nested GX command')
            begins[sid], active = row, sid
            interval['gx_commands'] += 1
            interval['command_lists'] += row['a'] == 1
            activity = activities.pop(row['thread'], None)
            if activity:
                relay = activity['relay']
                chains.append(dict(producer_thread=row['thread'], next_submission=sid,
                                   next_command_id=row['a'], irq=relay['a'],
                                   relay_sequence=relay['sequence'], activity_sequence=activity['sequence'],
                                   activity=activity['kind'], next_submission_sequence=seq,
                                   same_vblank_interval=relay['frame'] == frame,
                                   timer0_ticks_to_next_submission=row['timer0_ticks'] - relay['timer0_ticks'],
                                   run_observed=activity.get('run_observed', False),
                                   wait_block_observed=activity.get('wait_block_observed', False),
                                   wait_ready_observed=activity.get('wait_ready_observed', False)))
        elif kind == 'GXEnd':
            begin = begins.get(sid)
            if begin is None or active != sid:
                if not allow_partial:
                    raise ValueError('GXEnd without matching active GXBegin')
                unmatched_ends += 1
                continue
            active = None
            if row['c'] not in (0, 1):
                raise ValueError('invalid counter availability')
            commands.append(dict(submission=sid, frame=begin['frame'], command_id=begin['a'],
                                 producer_thread=begin['thread'], gsp_recipient=begin['object'],
                                 begin_sequence=begin['sequence'], end_sequence=seq,
                                 list_address=begin['d'] >> 32 if begin['a'] == 1 else None,
                                 list_size=begin['d'] & 0xffffffff if begin['a'] == 1 else None,
                                 ring_index=begin['b'] & 255, ring_count=(begin['b'] >> 8) & 255,
                                 chosen_slot=begin['c'] >> 32, active_recipient=begin['c'] & 0xffffffff,
                                 draws=row['a'] if row['c'] else None,
                                 gpu_vertices=row['b'] if row['c'] else None))
            interval = intervals[begin['frame']]
            if row['c']:
                interval['draws'] += row['a']
                interval['gpu_vertices'] += row['b']
            else:
                interval['counters_available'] = False
        elif kind == 'IRQRequest':
            if row['a'] == 5:
                interval['p3d_requests'] += 1
            elif row['a'] == 4:
                interval['ppf_requests'] += 1
            key = (row['thread'], row['object'], row['a'])
            if key in requests:
                raise ValueError('IRQ request overwritten before relay')
            requests[key] = row
        elif kind == 'Relay':
            request = requests.pop((row['thread'], row['object'], row['a']), None)
            relay_outcomes[str(row['b'])] += 1
            if request:
                irq_pairs.append(dict(irq=row['a'], submission=sid,
                                      request_sequence=request['sequence'], relay_sequence=seq,
                                      same_timer0_tick=request['timer0_ticks'] == row['timer0_ticks'],
                                      inside_executing_gx=active == sid))
            if row['d'] & 1:
                pending_signals[row['object']] = row
        elif kind == 'EventSignal':
            relay = pending_signals.pop(row['object'], None)
            visible_signals.pop(row['object'], None)
            if relay:
                visible_signals[row['object']] = relay
        elif kind == 'EventClear':
            visible_signals.pop(row['object'], None)
            acquisitions.pop(row['object'], None)
        elif kind == 'EventAcquire':
            relay = visible_signals.pop(row['object'], None)
            acquisitions[row['object']] = (row['thread'], relay)
            if relay and relay['a'] in (4, 5):
                activities[row['thread']] = dict(relay=relay, sequence=seq, kind='EventAcquire',
                                                 wait_ready_observed=row['thread'] in ready)
            else:
                activities.pop(row['thread'], None)
            ready.pop(row['thread'], None)
        elif kind == 'Wake':
            acquired = acquisitions.pop(row['object'], None)
            if row['a'] == 0 and acquired and acquired[0] == row['thread'] and acquired[1]:
                relay = acquired[1]
                if relay['a'] in (4, 5):
                    activities[row['thread']] = dict(relay=relay, sequence=seq, kind='Wake',
                                                     wait_block_observed=row['thread'] in blocked)
            else:
                activities.pop(row['thread'], None)
            blocked.pop(row['thread'], None)
        elif kind in ('WaitReady', 'WaitBlock', 'WaitPoll'):
            wait_counts[kind] += 1
            if kind == 'WaitBlock':
                activities.pop(row['thread'], None)
                blocked[row['thread']] = seq
            elif kind == 'WaitReady':
                ready[row['thread']] = seq
        elif kind == 'Run':
            if row['thread'] in activities:
                activities[row['thread']]['run_observed'] = True
        elif kind == 'PicaIRQ':
            pica_writes[(row['a'], row['b'])] += 1
        elif kind == 'Swap' and row['a'] == 0:
            interval['top_swaps'] += 1
            top_swaps.append(row)
    ended = {entry['submission'] for entry in commands}
    open_commands = sorted(set(begins) - ended)
    if open_commands or unmatched_ends or requests:
        if not allow_partial:
            raise ValueError('unmatched command or IRQ boundaries')
        problems.append('unmatched command or IRQ boundaries')
    # This is a temporal successor lookup, not proof of newly produced work.
    list_commands = [entry for entry in commands if entry['command_id'] == 1]
    for chain in chains:
        successor = next((entry for entry in list_commands
                          if entry['producer_thread'] == chain['producer_thread']
                          and entry['begin_sequence'] >= chain['next_submission_sequence']), None)
        chain['subsequent_list_observed'] = successor is not None
        if successor:
            chain['subsequent_list_submission'] = successor['submission']
            chain['subsequent_list_draws'] = successor['draws']
            chain['subsequent_list_sequence'] = successor['begin_sequence']
    between_swaps = []
    for left, right in zip(top_swaps, top_swaps[1:]):
        selected = [entry for entry in commands if left['sequence'] < entry['begin_sequence'] < right['sequence']]
        available = all(entry['draws'] is not None for entry in selected)
        between_swaps.append(dict(begin_sequence=left['sequence'], end_sequence=right['sequence'],
                                  begin_frame=left['frame'], end_frame=right['frame'],
                                  gx_commands=len(selected), command_lists=sum(entry['command_id'] == 1 for entry in selected),
                                  draws=sum(entry['draws'] for entry in selected) if available else None,
                                  gpu_vertices=sum(entry['gpu_vertices'] for entry in selected) if available else None))
    for interval in intervals.values():
        if not interval['counters_available']:
            interval['draws'] = interval['gpu_vertices'] = None
    pairs = [pair for pair in irq_pairs if pair['irq'] in (4, 5)]
    return dict(draw_passes=draws,
                hid_updates=sum(row['kind'] == 20 for row in rows),
                sleep_requests=sum(row['kind'] == 21 for row in rows),
                capture=dict(metadata, usable_as_complete_capture=not problems,
                             limitations=problems, observed_vblank_intervals=len(boundaries),
                             current_timer_domains=domains), per_vblank=list(intervals.values()),
                per_list=[entry for entry in commands if entry['command_id'] == 1],
                between_consecutive_top_swaps=between_swaps,
                ring_observations=[dict(index=key[0], chosen_slot=key[1], count=key[2], occurrences=count)
                                   for key, count in sorted(Counter((entry['ring_index'], entry['chosen_slot'], entry['ring_count'])
                                                                 for entry in commands).items())],
                irq_delivery=dict(pica_register_writes=[dict(value=value, mask=mask, count=count)
                                                        for (value, mask), count in sorted(pica_writes.items())],
                                  completion_requests_paired=len(pairs),
                                  inside_executing_gx=sum(pair['inside_executing_gx'] for pair in pairs),
                                  same_timer0_tick=sum(pair['same_timer0_tick'] for pair in pairs),
                                  relay_outcomes=dict(relay_outcomes), examples=pairs[:example_limit]),
                direct_completion_to_producer=dict(observed_chains=len(chains),
                                                   same_vblank_interval=sum(chain['same_vblank_interval'] for chain in chains),
                                                   examples=chains[:example_limit]), waits=dict(wait_counts),
                unmatched=dict(open_commands=open_commands, gx_ends=unmatched_ends,
                               irq_requests_without_relay=len(requests)),
                interpretation=['Same-tick IRQ delivery describes emulator visibility, not measured hardware completion or proof of premature completion.',
                                'Chains join relay/event acquisition or wake to the same kernel thread issuing its next GX command; indirect handoffs are not inferred. Wake makes a thread ready; Run is reported separately.',
                                'Next GX and subsequent list are temporal observations, not proof of freshly produced work. Pointer reuse is not command-content equivalence; queued slots may predate the wake.',
                                'Compare list/draw/vertex volume between swaps and VBlanks with matched game progress; swaps every refresh do not establish equivalent work.',
                                'GPU vertex totals exclude CPU-path vertices. No cross-timer-domain deltas are calculated.'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--allow-partial', action='store_true')
    parser.add_argument('--limit', type=int, default=20, help='maximum IRQ and chain examples')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.limit < 0:
        parser.error('--limit must be nonnegative')
    try:
        metadata, rows = read_trace(args.trace.read_text())
        result = analyze(metadata, rows, args.allow_partial, args.limit)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    rendered = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(rendered)
    else:
        print(rendered, end='')


if __name__ == '__main__':
    main()
