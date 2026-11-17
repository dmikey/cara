#!/usr/bin/env python3
"""Validate and rank bounded successful surface-copy traces; no FPS extrapolation."""
import argparse
import json
from pathlib import Path


TOTAL_FIELDS = ('calls', 'guest_bytes', 'wall_ns', 'thread_cpu_ns', 'wall_samples',
                'cpu_samples', 'wall_failures', 'cpu_failures', 'integer_saturations')
LAYOUT_FIELDS = ('format', 'type', 'tiled', 'width', 'height', 'stride', 'scale', 'custom')


def integer(value, name, maximum=(1 << 64) - 1):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f'{name} must be an unsigned integer <= {maximum}')
    return value


def boolean(value, name):
    if type(value) is not bool:
        raise ValueError(f'{name} must be boolean')
    return value


def totals(value):
    if not isinstance(value, dict):
        raise ValueError('totals must be an object')
    result = {key: integer(value.get(key), key) for key in TOTAL_FIELDS}
    if result['integer_saturations']:
        raise ValueError('saturated totals cannot be exactly reconciled')
    for clock, samples, failures in (('wall_ns', 'wall_samples', 'wall_failures'),
                                      ('thread_cpu_ns', 'cpu_samples', 'cpu_failures')):
        if result[samples] + result[failures] != result['calls']:
            raise ValueError(f'{samples} + {failures} must equal calls')
        if not result[samples] and result[clock]:
            raise ValueError(f'{clock} requires a successful clock sample')
    return result


def add_totals(rows):
    return {key: sum(row[key] for row in rows) for key in TOTAL_FIELDS}


def layout(value):
    if not isinstance(value, dict):
        raise ValueError('surface layout must be an object')
    return {key: (boolean(value.get(key), key) if key in ('tiled', 'custom') else
                  integer(value.get(key), key, (1 << 32) - 1)) for key in LAYOUT_FIELDS}


def interval(value, name):
    if not isinstance(value, list) or len(value) != 2:
        raise ValueError(f'{name} must be a half-open address pair')
    start, end = (integer(x, name, (1 << 32) - 1) for x in value)
    if end <= start:
        raise ValueError(f'{name} must be nonempty and ordered')
    return start, end


DAMAGE_FIELDS = ('calls', 'actual_interval_bytes', 'hypothetical_interval_bytes',
                 'hypothetical_reduction_bytes', 'integer_saturations')


def signed_rect(value, name):
    if not isinstance(value, list) or len(value) != 4:
        raise ValueError(f'{name} must contain four signed coordinates')
    if any(type(x) is not int or not -(1 << 63) <= x < (1 << 63) for x in value):
        raise ValueError(f'{name} coordinates must be signed integers')


def provenance(value):
    if value is None:
        return None
    if not isinstance(value, dict):
        raise ValueError('invalidation provenance must be an object or null')
    if not integer(value.get('serial'), 'event serial'):
        raise ValueError('event serial must be positive')
    integer(value.get('frame'), 'event frame', (1 << 32) - 1)
    span = interval(value.get('range'), 'event range')
    origin = integer(value.get('origin'), 'origin', 6)
    gpu = boolean(value.get('gpu_owner'), 'gpu_owner')
    identity = value.get('owner_allocation_id')
    if identity is not None and not integer(identity, 'owner allocation ID'):
        raise ValueError('known owner allocation ID must be positive')
    if gpu:
        if origin == 1:
            raise ValueError('GPU owner cannot have CPU origin')
        owner = value.get('owner')
        layout(owner)
        address = integer(owner.get('address'), 'owner address', (1 << 32) - 1)
        end = integer(owner.get('end'), 'owner end', (1 << 32) - 1)
        if not address <= span[0] < span[1] <= end:
            raise ValueError('owning write must lie inside owner allocation')
        integer(owner.get('handle'), 'owner handle', (1 << 32) - 1)
        boolean(owner.get('registered'), 'owner registered')
    elif origin != 1 or identity is not None or value.get('owner') is not None:
        raise ValueError('CPU event must have CPU origin and no GPU owner')
    draw = value.get('draw')
    if draw is not None:
        if not isinstance(draw, dict) or not gpu or origin not in (2, 3):
            raise ValueError('draw context requires a draw origin and GPU owner')
        for key in ('mode', 'scale', 'color_mask', 'stencil_mask'):
            integer(draw.get(key), key, (1 << 32) - 1)
        for key in ('shadow', 'color_write', 'depth_write', 'has_stencil',
                    'stencil_test', 'depth_test', 'depth_mask',
                    'depth_surface_write', 'damage_narrowing_active'):
            boolean(draw.get(key), key)
        for key in ('viewport_damage', 'actual_damage', 'shader_scissor'):
            signed_rect(draw.get(key), key)
    return value


def damage_totals(value):
    if not isinstance(value, dict):
        raise ValueError('hypothetical damage totals must be an object')
    result = {key: integer(value.get(key), key) for key in DAMAGE_FIELDS}
    if result['integer_saturations']:
        raise ValueError('saturated hypothetical intervals cannot be exactly reconciled')
    if result['actual_interval_bytes'] != (result['hypothetical_interval_bytes'] +
                                           result['hypothetical_reduction_bytes']):
        raise ValueError('hypothetical interval arithmetic does not reconcile')
    if not result['calls'] and any(result.values()):
        raise ValueError('empty hypothetical ledger must have zero totals')
    return result


def summarize_damage(report):
    integer(report.get('unknown_gpu_owner_events'), 'unknown GPU owner events')
    damage = report.get('hypothetical_damage')
    if not isinstance(damage, dict) or damage.get('scope') != 'top_level_invalidation_logical_intervals_not_copy_traffic':
        raise ValueError('unexpected hypothetical damage scope')
    full, overflow = damage_totals(damage.get('totals')), damage_totals(damage.get('overflow'))
    if full['calls']:
        low = integer(damage.get('actual_min_frame'), 'damage min frame', (1 << 32) - 1)
        high = integer(damage.get('actual_max_frame'), 'damage max frame', (1 << 32) - 1)
        if not report['window_start_inclusive'] <= low <= high < report['window_end_exclusive']:
            raise ValueError('hypothetical event frames must lie inside the trace window')
    elif damage.get('actual_min_frame') is not None or damage.get('actual_max_frame') is not None:
        raise ValueError('empty hypothetical ledger must have null frame bounds')
    buckets = damage.get('buckets')
    if not isinstance(buckets, list) or len(buckets) > report['bucket_capacity']:
        raise ValueError('invalid hypothetical damage bucket count')
    if overflow['calls'] and len(buckets) != report['bucket_capacity']:
        raise ValueError('hypothetical overflow requires full bucket capacity')
    rows, keys = [], set()
    for bucket in buckets:
        if not isinstance(bucket, dict):
            raise ValueError('hypothetical bucket must be an object')
        origin = integer(bucket.get('origin'), 'origin', 6)
        eligible = integer(bucket.get('eligibility'), 'eligibility', 7)
        gpu = boolean(bucket.get('gpu_owner'), 'gpu_owner')
        owner = layout(bucket.get('owner_layout')) if gpu else bucket.get('owner_layout')
        if (not gpu and (origin != 1 or owner is not None)) or (gpu and origin == 1):
            raise ValueError('hypothetical bucket owner/origin mismatch')
        if eligible != 0 and (not gpu or origin not in (2, 3)):
            raise ValueError('scissor eligibility requires a GPU draw owner')
        count = damage_totals(bucket.get('totals'))
        if not count['calls']:
            raise ValueError('occupied hypothetical bucket must contain an event')
        if eligible != 6 and count['hypothetical_reduction_bytes']:
            raise ValueError('ineligible event cannot claim hypothetical reduction')
        descriptor = dict(origin=origin, eligibility=eligible, gpu_owner=gpu, owner_layout=owner)
        key = json.dumps(descriptor, sort_keys=True)
        if key in keys:
            raise ValueError('duplicate hypothetical damage bucket')
        keys.add(key)
        rows.append(dict(descriptor, **count))
    if {key: sum(row[key] for row in rows + [overflow]) for key in DAMAGE_FIELDS} != full:
        raise ValueError('hypothetical buckets + overflow do not reconcile')
    rows.sort(key=lambda r: -r['actual_interval_bytes'])
    return dict(scope=damage['scope'], totals=full, overflow=overflow, rows=rows,
                limitation='Hypothetical logical invalidation intervals; not copied bytes, traffic or savings.')


CENSUS_FIELDS = ('calls', 'proposal_found', 'proposal_changed', 'same_rect_changed',
    'same_rect_depth_compatible_changed', 'guarded_proposal_changed', 'legacy_full_valid',
    'legacy_viewport_valid', 'proposed_full_valid', 'proposed_viewport_valid',
    'feedback_risk', 'mip_or_special_risk', 'candidate_visits', 'compatible_visits', 'integer_saturations')
FEEDBACK_FIELDS = ('calls', 'source_level0_texels', 'source_level0_gl_bytes', 'allocation_attempts',
    'cold_allocations', 'footprint_failures', 'wall_ns', 'thread_cpu_ns', 'wall_samples',
    'cpu_samples', 'wall_failures', 'cpu_failures', 'integer_saturations', 'timing_integer_saturations')


def params(value):
    if not isinstance(value, dict):
        raise ValueError('query params must be an object')
    for key in ('format', 'type', 'width', 'height', 'stride', 'scale', 'address', 'end', 'size'):
        integer(value.get(key), key, (1 << 32) - 1)
    boolean(value.get('tiled'), 'query tiled')
    if value['end'] - value['address'] != value['size']:
        raise ValueError('query address/end/size disagree')
    return value


def view(value):
    if value is None:
        return None
    if not isinstance(value, dict):
        raise ValueError('census view must be an object or null')
    identity = value.get('allocation_id')
    if identity is not None and not integer(identity, 'census allocation ID'):
        raise ValueError('known census identity must be positive')
    for key in ('handle', 'max_level'):
        integer(value.get(key), key, (1 << 32) - 1)
    for key in ('registered', 'custom'):
        boolean(value.get(key), key)
    params(value.get('params'))
    return value


def rectangle(value):
    if not isinstance(value, list) or len(value) != 4:
        raise ValueError('census rectangle must have four coordinates')
    for item in value:
        integer(item, 'census rectangle coordinate', (1 << 32) - 1)
    return value


def nested_totals(value, fields):
    if not isinstance(value, dict):
        raise ValueError('nested trace totals must be an object')
    result = {key: integer(value.get(key), key) for key in fields}
    if result['integer_saturations'] or result.get('timing_integer_saturations', 0):
        raise ValueError('nested trace saturated; exact reconciliation unavailable')
    return result


def nested_frame_bounds(report, child, calls):
    if calls:
        low = integer(child.get('actual_min_frame'), 'nested min frame', (1 << 32) - 1)
        high = integer(child.get('actual_max_frame'), 'nested max frame', (1 << 32) - 1)
        if not report['window_start_inclusive'] <= low <= high < report['window_end_exclusive']:
            raise ValueError('nested trace frames outside window')
    elif child.get('actual_min_frame') is not None or child.get('actual_max_frame') is not None:
        raise ValueError('empty nested trace requires null frame bounds')


def census_row(e):
    if not isinstance(e, dict):
        raise ValueError('census detail must be an object')
    original, effective = params(e.get('original')), params(e.get('effective'))
    if original['format'] != 0 or original['type'] != 0:
        raise ValueError('framebuffer census scope requires ordinary RGBA8 color')
    legacy, proposal, depth = view(e.get('legacy')), view(e.get('proposal')), view(e.get('depth'))
    for key in ('legacy_rect', 'legacy_query_rect', 'proposal_rect', 'depth_rect'):
        rectangle(e.get(key))
    boolean(e.get('depth_requested'), 'depth requested')
    flags = {key: boolean(e.get(key), key) for key in ('changed', 'same_rect', 'depth_compatible',
        'legacy_full_valid', 'legacy_viewport_valid', 'proposed_full_valid', 'proposed_viewport_valid',
        'feedback_risk', 'mip_or_special_risk')}
    if not legacy and (flags['legacy_full_valid'] or flags['legacy_viewport_valid']):
        raise ValueError('missing legacy cannot be valid')
    if not proposal and any(flags[key] for key in ('changed', 'same_rect', 'depth_compatible',
                                                  'proposed_full_valid', 'proposed_viewport_valid', 'feedback_risk')):
        raise ValueError('missing proposal cannot carry proposal flags')
    if proposal:
        q = proposal['params']
        if not proposal['registered'] or proposal['custom'] or q['scale'] != effective['scale']:
            raise ValueError('proposal violates registered/noncustom/exact-scale guards')
        if q['format'] != effective['format'] or q['type'] != 0 or q['tiled'] != effective['tiled']:
            raise ValueError('proposal format/type/tiling mismatch')
        if not q['address'] <= effective['address'] < effective['end'] <= q['end']:
            raise ValueError('proposal does not contain full effective query')
        if flags['same_rect'] != (e['proposal_rect'] == e['legacy_rect']):
            raise ValueError('same-rectangle flag disagrees with recorded rectangles')
        if flags['depth_compatible'] != (not depth or e['proposal_rect'] == e['depth_rect']):
            raise ValueError('depth-compatibility flag disagrees with recorded rectangles')
        if legacy and legacy['allocation_id'] is not None and proposal['allocation_id'] is not None:
            if flags['changed'] != (legacy['allocation_id'] != proposal['allocation_id']):
                raise ValueError('changed flag disagrees with known identities')
    samplers = e.get('samplers')
    if not isinstance(samplers, list) or len(samplers) != 3:
        raise ValueError('census requires exactly three sampler descriptors')
    feedback = False
    risk = bool(proposal and proposal['max_level'])
    for unit, sample in enumerate(samplers):
        if not isinstance(sample, dict):
            raise ValueError('sampler descriptor must be an object')
        enabled = boolean(sample.get('enabled'), 'sampler enabled')
        available = boolean(sample.get('prediction_available'), 'prediction available')
        exact = boolean(sample.get('exact_proposal_layout'), 'exact proposal layout')
        raw = integer(sample.get('raw_type'), 'raw sampler type', (1 << 32) - 1)
        effective_type = integer(sample.get('effective_type'), 'effective sampler type', (1 << 32) - 1)
        maximum = integer(sample.get('max_level'), 'sampler max level', 15)
        if effective_type != (raw if unit == 0 else 0):
            raise ValueError('sampler effective type does not match unit semantics')
        raw_desc = {key: integer(sample.get(key), key, (1 << 32) - 1)
                    for key in ('address', 'width', 'height', 'format')}
        expected_available = bool(enabled and raw_desc['width'] and raw_desc['height'] and
            not raw_desc['width'] % 8 and not raw_desc['height'] % 8 and
            raw_desc['format'] <= 13 and raw_desc['address'])
        if available != expected_available:
            raise ValueError('sampler prediction availability disagrees with raw descriptor')
        if available:
            if not enabled:
                raise ValueError('disabled sampler cannot predict a proposal')
            p = params(sample.get('params'))
            if any(p[key] != raw_desc[key] for key in raw_desc):
                raise ValueError('sampler prediction params disagree with raw descriptor')
            expected = bool(proposal and all(p[key] == proposal['params'][key] for key in
                ('address', 'width', 'height', 'stride', 'format', 'tiled')))
            if exact != expected:
                raise ValueError('exact sampler proposal-layout flag mismatch')
        elif sample.get('params') is not None or exact:
            raise ValueError('unavailable sampler prediction must remain unknown')
        feedback |= exact
        risk |= enabled and (not available or effective_type != 0 or maximum != 0)
    if flags['feedback_risk'] != feedback or flags['mip_or_special_risk'] != bool(risk):
        raise ValueError('sampler risk flags disagree with descriptors')
    row = dict.fromkeys(CENSUS_FIELDS, 0)
    row.update(calls=1, proposal_found=int(proposal is not None), proposal_changed=int(flags['changed']),
        same_rect_changed=int(flags['changed'] and flags['same_rect']),
        same_rect_depth_compatible_changed=int(flags['changed'] and flags['same_rect'] and flags['depth_compatible']),
        guarded_proposal_changed=int(flags['changed'] and flags['same_rect'] and flags['depth_compatible'] and
            not feedback and not risk))
    for key in ('legacy_full_valid', 'legacy_viewport_valid', 'proposed_full_valid',
                'proposed_viewport_valid', 'feedback_risk', 'mip_or_special_risk'):
        row[key] = int(flags[key])
    for key in ('candidate_visits', 'compatible_visits'):
        row[key] = integer(e.get(key), key)
    if row['compatible_visits'] > row['candidate_visits']:
        raise ValueError('compatible visits exceed all candidate visits')
    return row, (int(flags['changed']) | (int(flags['same_rect']) << 1) |
        (int(flags['depth_compatible']) << 2) | (int(feedback) << 3) | (int(bool(risk)) << 4))


def summarize_census(report):
    c = report['framebuffer_census']
    if not isinstance(c, dict) or type(c.get('version')) is not int or c['version'] != 1:
        raise ValueError('unsupported framebuffer census version')
    full = nested_totals(c.get('totals'), CENSUS_FIELDS)
    overflow = nested_totals(c.get('detail_overflow'), CENSUS_FIELDS)
    for value in (full, overflow):
        if any(value[key] > value['calls'] for key in CENSUS_FIELDS if key not in
               ('calls', 'candidate_visits', 'compatible_visits', 'integer_saturations')):
            raise ValueError('census subset counter exceeds calls')
    nested_frame_bounds(report, c, full['calls'])
    cap = integer(c.get('detail_capacity'), 'census detail capacity')
    details, classes = c.get('details'), c.get('classes')
    if not cap or not isinstance(details, list) or len(details) != min(cap, full['calls']):
        raise ValueError('invalid census chronological detail count')
    if integer(c.get('class_saturations'), 'class saturations'):
        raise ValueError('saturated census class counts')
    if not isinstance(classes, list) or len(classes) != 32:
        raise ValueError('census requires 32 exhaustive classification counts')
    classes = [integer(x, 'class count') for x in classes]
    if sum(classes) != full['calls']:
        raise ValueError('census classes do not reconcile with calls')
    rows = []
    recorded_classes = [0] * 32
    for event in details:
        frame = integer(event.get('frame'), 'census frame', (1 << 32) - 1)
        if not c['actual_min_frame'] <= frame <= c['actual_max_frame']:
            raise ValueError('census detail frame outside observed bounds')
        row, category = census_row(event)
        rows.append(row); recorded_classes[category] += 1
    if any(a > b for a, b in zip(recorded_classes, classes)):
        raise ValueError('census details exceed aggregate class counts')
    if {key: sum(r[key] for r in rows) + overflow[key] for key in CENSUS_FIELDS} != full:
        raise ValueError('census details + overflow do not reconcile')
    return dict(totals=full, detail_overflow=overflow, classes=classes,
                limitation='Read-only structural proposal; sampler risk is conservative, not a proven avoided copy.')


def summarize_feedback(report):
    f = report['feedback']
    if not isinstance(f, dict) or type(f.get('version')) is not int or f['version'] != 1:
        raise ValueError('unsupported feedback version')
    full = nested_totals(f.get('totals'), FEEDBACK_FIELDS)
    overflow = nested_totals(f.get('detail_overflow'), FEEDBACK_FIELDS)
    for value in (full, overflow):
        if not value['cold_allocations'] <= value['allocation_attempts'] <= value['calls'] or value['footprint_failures'] > value['calls']:
            raise ValueError('feedback allocation/footprint counters exceed calls')
        for samples, failures in (('wall_samples', 'wall_failures'), ('cpu_samples', 'cpu_failures')):
            if value[samples] + value[failures] != value['calls']:
                raise ValueError('feedback clock coverage does not reconcile')
    nested_frame_bounds(report, f, full['calls'])
    cap = integer(f.get('detail_capacity'), 'feedback capacity')
    details = f.get('details')
    if not cap or not isinstance(details, list) or len(details) != min(cap, full['calls']):
        raise ValueError('invalid feedback chronological detail count')
    rows = []
    for event in details:
        if not isinstance(event, dict):
            raise ValueError('feedback detail must be an object')
        frame = integer(event.get('frame'), 'feedback frame', (1 << 32) - 1)
        if not f['actual_min_frame'] <= frame <= f['actual_max_frame']:
            raise ValueError('feedback frame outside observed bounds')
        if view(event.get('source')) is None:
            raise ValueError('feedback must have a source view')
        row = dict.fromkeys(FEEDBACK_FIELDS, 0)
        row['calls'] = 1
        row['source_level0_texels'] = integer(event.get('source_level0_texels'), 'feedback texels')
        size = event.get('source_level0_gl_bytes')
        if size is None: row['footprint_failures'] = 1
        else: row['source_level0_gl_bytes'] = integer(size, 'feedback source bytes')
        cold = boolean(event.get('allocation_attempt'), 'allocation attempt')
        handle = integer(event.get('copy_handle'), 'feedback copy handle', (1 << 32) - 1)
        row['allocation_attempts'] = int(cold); row['cold_allocations'] = int(cold and handle != 0)
        for key, samples, failures in (('wall_ns', 'wall_samples', 'wall_failures'),
                                       ('thread_cpu_ns', 'cpu_samples', 'cpu_failures')):
            duration = event.get(key)
            if duration is None: row[failures] = 1
            else: row[key] = integer(duration, key); row[samples] = 1
        rows.append(row)
    if {key: sum(r[key] for r in rows) + overflow[key] for key in FEEDBACK_FIELDS} != full:
        raise ValueError('feedback details + overflow do not reconcile')
    return dict(totals=full, detail_overflow=overflow,
                limitation='Returned full level-zero feedback snapshots; host call time, not GPU completion or physical traffic.')


def summarize(report, rank='time', limit=64):
    if rank not in ('time', 'cpu', 'calls', 'bytes') or type(limit) is not int or limit <= 0:
        raise ValueError('invalid ranking or row limit')
    if not isinstance(report, dict) or report.get('kind') != 'returned_validate_surface_copies':
        raise ValueError('expected a returned surface-validation-copy report')
    if type(report.get('schema_version')) is not int or report['schema_version'] not in (1, 2):
        raise ValueError('unsupported schema version')
    if report.get('enabled') is not True or report.get('common_counters_required') is not False:
        raise ValueError('expected enabled counters-independent tracing')
    if report.get('frame_clock') != 'VideoCore::GetCurrentFrame':
        raise ValueError('unexpected frame clock')
    start = integer(report.get('window_start_inclusive'), 'start', (1 << 32) - 1)
    end = integer(report.get('window_end_exclusive'), 'end', (1 << 32) - 1)
    integer(report.get('written_frame'), 'written_frame', (1 << 32) - 1)
    caps = {key: integer(report.get(key), key) for key in
            ('detail_capacity', 'identity_capacity', 'bucket_capacity')}
    if any(not x for x in caps.values()):
        raise ValueError('trace capacities must be positive')
    for key in ('allocation_events', 'untracked_allocations', 'unknown_identity_uses',
                'validation_invocations_all_frames', 'invalidation_calls_all_frames'):
        integer(report.get(key), key)
    full = totals(report.get('totals'))
    detail_overflow = totals(report.get('detail_overflow'))
    bucket_overflow = totals(report.get('bucket_overflow'))
    if full['calls']:
        low = integer(report.get('actual_min_frame'), 'min_frame', (1 << 32) - 1)
        high = integer(report.get('actual_max_frame'), 'max_frame', (1 << 32) - 1)
        if not start <= low <= high < end:
            raise ValueError('observed copy frames must lie within the trace window')
    elif report.get('actual_min_frame') is not None or report.get('actual_max_frame') is not None:
        raise ValueError('an empty trace must have null frame endpoints')
    details, buckets = report.get('details'), report.get('buckets')
    if not isinstance(details, list) or not isinstance(buckets, list):
        raise ValueError('details and buckets must be arrays')
    if len(details) != min(caps['detail_capacity'], full['calls']):
        raise ValueError('detail count does not match chronological capture capacity')
    if len(buckets) > caps['bucket_capacity']:
        raise ValueError('bucket count exceeds capacity')
    if bucket_overflow['calls'] and len(buckets) != caps['bucket_capacity']:
        raise ValueError('bucket overflow requires all bucket slots occupied')
    detail_totals, invalidations, unsafe_sources = [], {0: 0, 1: 0, 2: 0}, 0
    for event in details:
        if not isinstance(event, dict):
            raise ValueError('detail must be an object')
        frame = integer(event.get('frame'), 'detail frame', (1 << 32) - 1)
        if not low <= frame <= high:
            raise ValueError('detail frame outside observed bounds')
        for key in ('invocation', 'fragment'):
            if not integer(event.get(key), key):
                raise ValueError(f'{key} must be positive')
        requested = interval(event.get('requested'), 'requested')
        invalid = interval(event.get('invalid'), 'invalid')
        actual = interval(event.get('actual'), 'actual')
        if not requested[0] <= invalid[0] < invalid[1] <= requested[1]:
            raise ValueError('invalid fragment must be inside the validation request')
        if max(actual[0], invalid[0]) >= min(actual[1], invalid[1]):
            raise ValueError('returned copy does not advance the invalid fragment')
        if integer(event.get('guest_bytes'), 'detail bytes') != actual[1] - actual[0]:
            raise ValueError('detail bytes must equal actual copied interval length')
        fill = boolean(event.get('fill'), 'fill')
        unsafe_sources += not boolean(event.get('source_valid_before_copy'), 'source validity')
        for side in ('src', 'dst'):
            source = event.get(side)
            layout(source)
            address = integer(source.get('address'), 'surface address', (1 << 32) - 1)
            last = integer(source.get('end'), 'surface end', (1 << 32) - 1)
            if not address <= actual[0] < actual[1] <= last:
                raise ValueError('copy interval must lie inside both surface allocations')
            integer(source.get('handle'), 'handle', (1 << 32) - 1)
            boolean(source.get('registered'), 'registered')
            identity = source.get('allocation_id')
            created = source.get('created_frame')
            if identity is None:
                if created is not None:
                    raise ValueError('unknown allocation must have unknown creation frame')
            else:
                if not integer(identity, 'allocation_id'):
                    raise ValueError('known allocation ID must be positive')
                integer(created, 'created_frame', (1 << 32) - 1)
            serial = source.get('last_observed_invalidation_serial')
            reason = integer(source.get('invalidation_reason'), 'invalidation reason', 2)
            if serial is None:
                if reason:
                    raise ValueError('unknown invalidation must have unknown reason')
            elif not integer(serial, 'invalidation serial') or not reason:
                raise ValueError('observed invalidation requires positive serial and reason')
            if report['schema_version'] == 2:
                if 'last_invalidation_event' not in source or 'last_owner_write' not in source:
                    raise ValueError('v2 surface is missing correlated provenance')
                last_event = provenance(source['last_invalidation_event'])
                own = provenance(source['last_owner_write'])
                if (last_event is None) != (serial is None) or (last_event and last_event['serial'] != serial):
                    raise ValueError('legacy invalidation serial and correlated event disagree')
                if last_event and ((reason == 2) != last_event['gpu_owner']):
                    raise ValueError('legacy invalidation reason and GPU owner disagree')
                if own and (identity is None or not own['gpu_owner'] or own['owner_allocation_id'] != identity):
                    raise ValueError('own-write identity must match the surface lifetime')
            if side == 'dst':
                invalidations[reason] += 1
            rectangle = event.get(side + '_scaled_rect')
            if side == 'src' and fill:
                if rectangle is not None:
                    raise ValueError('fill source has no texture rectangle')
            elif not isinstance(rectangle, list) or len(rectangle) != 4:
                raise ValueError('scaled rectangle must contain four coordinates')
            else:
                for coordinate in rectangle:
                    integer(coordinate, 'rectangle coordinate', (1 << 32) - 1)
        row = dict.fromkeys(TOTAL_FIELDS, 0)
        row.update(calls=1, guest_bytes=event['guest_bytes'])
        for field, samples, failures in (('wall_ns', 'wall_samples', 'wall_failures'),
                                          ('thread_cpu_ns', 'cpu_samples', 'cpu_failures')):
            elapsed = event.get(field)
            if elapsed is None:
                row[failures] = 1
            else:
                row[field] = integer(elapsed, field)
                row[samples] = 1
        detail_totals.append(row)
    if add_totals(detail_totals + [detail_overflow]) != full:
        raise ValueError('chronological details + overflow do not reconcile with full totals')
    rows, keys = [], set()
    for bucket in buckets:
        if not isinstance(bucket, dict):
            raise ValueError('bucket must be an object')
        descriptor = dict(fill=boolean(bucket.get('fill'), 'bucket fill'),
                          src=layout(bucket.get('src')), dst=layout(bucket.get('dst')))
        key = json.dumps(descriptor, sort_keys=True)
        if key in keys:
            raise ValueError('duplicate aggregate layout bucket')
        keys.add(key)
        values = totals(bucket.get('totals'))
        if not values['calls']:
            raise ValueError('occupied bucket must contain a returned copy')
        rows.append(dict(descriptor, **values))
    if add_totals(rows + [bucket_overflow]) != full:
        raise ValueError('layout buckets + overflow do not reconcile with full totals')
    field = dict(time='wall_ns', cpu='thread_cpu_ns', calls='calls', bytes='guest_bytes')[rank]
    rows.sort(key=lambda row: (-row[field], json.dumps(row, sort_keys=True)))
    result = dict(scope='successful_ValidateSurface_CopySurface_returns_only',
                timing='instrumented_host_enqueue_or_wait_not_gpu_completion',
                window=dict(start_inclusive=start, end_exclusive=end),
                totals=full, bucket_overflow=bucket_overflow, detail_overflow=detail_overflow,
                aggregate_buckets=len(rows), rows=rows[:limit], rank=rank,
                chronological_details=dict(count=len(details), unbiased_sample=False,
                    destination_last_observed_reason=invalidations,
                    invalid_sources_before_copy=unsafe_sources),
                limitations=['No skipped-copy or cache-reuse safety conclusion follows from counts.',
                    'Chronological details can omit later addresses and invalidation histories.',
                    'Logical guest bytes are copied spans, not host memory-bus traffic.',
                    'Clock and tracing overhead remain; no speedup or FPS estimate is inferred.'])
    if report['schema_version'] == 2:
        result['hypothetical_damage'] = summarize_damage(report)
    if 'framebuffer_census' in report:
        result['framebuffer_census'] = summarize_census(report)
    if 'feedback' in report:
        result['feedback'] = summarize_feedback(report)
    return result


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--rank', choices=('time', 'cpu', 'calls', 'bytes'), default='time')
    parser.add_argument('--limit', type=int, default=64)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        report = json.loads(args.report.read_text(), object_pairs_hook=unique_object)
        result = summarize(report, args.rank, args.limit)
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(2, f'Invalid surface-copy report: {error}\n')
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
