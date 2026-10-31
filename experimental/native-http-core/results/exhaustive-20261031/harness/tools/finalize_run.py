#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Apply the recorded exploratory selection rule, then independent validation/pairs."""
import json
from pathlib import Path
import shutil
import subprocess

import sweep
from analyze_sweep import analyze

data = analyze(sweep.OUT)
selected = ['lazy_rate_json']
decisions = []
for row in data['screening'] + data['extra_screening']:
    flag = row.get('flag') or row['label'].removeprefix('extra-')
    accepted = row['delta_percent'] >= 2 and row['qps'] > max(row['control_before'], row['control_after'])
    decisions.append({**row, 'selected': accepted})
    if accepted:
        selected.append(flag)
if 'request_pool_native' in selected and 'request_pool' in selected:
    selected.remove('request_pool')
if 'matched_pool' in selected and 'lazy_route_opts' not in selected:
    selected.append('lazy_route_opts')

def sample(label):
    rows = json.loads((sweep.OUT / label / 'results.json').read_text())
    assert len(rows) == 1
    return rows[0]

post = {opt: sample('compiler-local-control-o2' if opt == 'O3-lto-local'
                   else 'compiler-control-o2')['qps'] for opt in ['O3', 'O3-lto', 'O3-lto-local']}
pre_floor = max(sample('extra-control-0')['qps'], sample('extra-control-2')['qps'])
compilers = {opt: sample('compiler-'+opt)['qps'] for opt in post}
eligible = {opt: qps for opt, qps in compilers.items()
            if qps > max(pre_floor, post[opt]) and qps >= 1.02 * post[opt]}
optimization = max(eligible, key=eligible.get) if eligible else 'O2'
directives = ' '.join('apisix_core_'+flag+' on;' for flag in selected)
decision = {'flags': selected, 'directives': directives, 'optimization': optimization,
            'candidates': decisions, 'compiler_qps': compilers, 'compiler_post_o2_qps': post,
            'compiler_pre_floor_qps': pre_floor,
            'note': 'Exploratory selection, not significance testing. Independent final rounds are all reported.'}
with (sweep.OUT/'selection-final.json').open('x') as output:
    json.dump(decision, output, indent=2)
print(json.dumps(decision), flush=True)
if optimization != 'O2':
    sweep.rebuild('selected-build', optimization)
for name in ['ngx_http_apisix_core_module.so', 'build.json']:
    shutil.copyfile(Path('/opt/apisix-core-module/lib')/name, sweep.OUT/('selected-'+name))

for profile in ['default', 'servlet', 'tail', 'encoded', 'trusted', 'untrusted']:
    sweep.run('selected-core-'+profile, 'tests/test_core.py', directives=directives,
              stable=True, extra={'APISIX_CORE_PROFILE':profile})
# This historical fixture asserts the research branch's normalized forwarding
# policy. Stable 3.19.0 preserves a raw dot-segment URI upstream; test_single
# separately checks the common stable-source plugin benchmark contract.
sweep.run('selected-demo', 'tests/test_demo.py', directives=directives)
sweep.run('selected-single', 'tests/test_single.py', directives=directives, stable=True)
sweep.bench('final-round-1', ['metrics','auth','limit'], directives, ('lua','native'))
sweep.bench('final-round-2', ['metrics','auth','limit'], directives, ('native','lua'))
subprocess.run(['python3', str(sweep.HERE/'tools/sweep.py'), 'diagnostics',
                '--directives', directives], check=True)
