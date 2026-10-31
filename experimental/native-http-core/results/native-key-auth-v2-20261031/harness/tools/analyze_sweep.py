#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Recompute screening and independent paired results from retained raw JSON."""
import argparse
import json
from pathlib import Path
import statistics


def analyze(root):
    def sample(label):
        rows = json.loads((root / label / 'results.json').read_text())
        assert len(rows) == 1
        return rows[0]

    groups = [('metrics', ['batch_vars', 'http_var_plan', 'lazy_route_opts',
                          'route_candidates', 'request_pool', 'lazy_meta_matches',
                          'phase_index', 'phase_context', 'plain_uri', 'native_var_writes']),
              ('auth', ['request_header_flags', 'header_scratch', 'sdk_cache_replay'])]
    screening = []
    for prefix, flags in groups:
        for i, flag in enumerate(flags):
            before = 'control-' + prefix + '-' + str(i // 3 * 3)
            after = 'control-' + prefix + '-' + str(min((i // 3 + 1) * 3, len(flags)))
            label = 'screen-' + flag
            if not all((root / name / 'results.json').exists() for name in [before, after, label]):
                continue
            row, left, right = sample(label), sample(before), sample(after)
            reference = statistics.mean([left['qps'], right['qps']])
            screening.append({'flag': flag, 'prefix': prefix, 'qps': row['qps'],
                              'control_before': left['qps'], 'control_after': right['qps'],
                              'delta_percent': (row['qps'] / reference - 1) * 100,
                              'worker_cpu_us_per_request': row['worker_cpu_us_per_request'],
                              'rss_peak_bytes': row['rss_peak_bytes']})
    extras = []
    for label, before, after in [
        ('extra-request_pool_native', 'extra-control-0', 'extra-control-2'),
        ('extra-balancer_peer_plan', 'extra-control-0', 'extra-control-2'),
        ('extra-matched_pool', 'extra-matched-control-0', 'extra-matched-control-1'),
    ]:
        if not all((root / name / 'results.json').exists() for name in [label, before, after]):
            continue
        row, left, right = sample(label), sample(before), sample(after)
        extras.append({'label': label, 'qps': row['qps'],
                       'control_before': left['qps'], 'control_after': right['qps'],
                       'delta_percent': (row['qps'] / statistics.mean([left['qps'], right['qps']]) - 1) * 100,
                       'worker_cpu_us_per_request': row['worker_cpu_us_per_request']})
    pairs = []
    for path in sorted(root.glob('final-*/results.json')):
        rows = json.loads(path.read_text())
        for prefix in ['metrics', 'auth', 'limit']:
            modes = {row['mode']: row for row in rows if row['prefix'] == prefix}
            if set(modes) != {'lua', 'native'}:
                continue
            lua, native = modes['lua'], modes['native']
            pairs.append({'round': path.parent.name, 'prefix': prefix,
                          'lua_qps': lua['qps'], 'native_qps': native['qps'],
                          'delta_percent': (native['qps'] / lua['qps'] - 1) * 100,
                          'lua_cpu_us': lua['worker_cpu_us_per_request'],
                          'native_cpu_us': native['worker_cpu_us_per_request']})
    return {'screening_note': 'Single-sample exploratory selection; shared flanking controls; '
                             'not independent causal effects or significance tests.',
            'screening': screening, 'extra_screening': extras, 'final_pairs': pairs}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.root), indent=2))
