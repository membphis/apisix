#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile

parser = argparse.ArgumentParser(description='Verify subtraction arithmetic and archived evidence without starting NGINX.')
parser.add_argument('results', type=Path)
args = parser.parse_args()
script = Path(__file__).with_name('analyze.py')
spec = importlib.util.spec_from_file_location('subtraction_analysis', script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
exact = module.fit([(k, 10 + 2 * k) for k in [1, 2, 4, 8]])
assert abs(exact['intercept_us'] - 10) < 1e-12
assert abs(exact['slope_us_per_iteration'] - 2) < 1e-12
assert exact['r_squared'] == 1 and exact['loo_max_abs_relative_prediction_error_percent'] < 1e-12
ids = ['nginx-before', 'lua-none-before', 'native-none-before', 'native-original', 'lua-original']
for index, k in enumerate([0, 1, 2, 4, 8]):
    ids.extend(f'{mode}-loop-{k}' for mode in (['lua', 'native'] if index % 2 == 0 else ['native', 'lua']))
ids += ['native-none-after', 'lua-none-after', 'nginx-after']
assert len(ids) == 18
rows = []
for name in ids:
    if name.startswith('nginx-'):
        cpu = 20
    elif '-none-' in name:
        cpu = 50 if name.startswith('lua-') else 40
    elif name.endswith('original'):
        cpu = 70 if name.startswith('lua-') else 55
    else:
        k = int(name.rsplit('-', 1)[1])
        cpu = (60 + 10 * k if name.startswith('lua-') else 50 + 5 * k) if k else (52 if name.startswith('lua-') else 41)
    rows.append({'id': name, 'duration_seconds': 60, 'requests': 180000, 'cpu_us_per_request': cpu,
                 'worker_cpu_utilization': cpu * .003, 'one_upstream_per_request': True, 'error_rate': 0,
                 'load': {'completed': 180000, 'target_requests': 180000, 'errors': [],
                          'all_slots_completed': True, 'schedule_drained_within_one_second': True,
                          'wire_latency_us': {'mean': 1, 'p99': 2}, 'scheduled_latency_us': {'p99': 3}}})
environment = {'plan': [{'id': name} for name in ids], 'rate': 3000, 'normal_gc': True,
               'clock_probes': False, 'allocator_probes': False, 'gc_probes': False}
with tempfile.TemporaryDirectory(prefix='subtraction-arithmetic-') as directory:
    p = Path(directory)
    (p / 'environment.json').write_text(json.dumps(environment))
    (p / 'results.json').write_text(json.dumps(rows))
    result = module.analyze(p)
    assert result['addon_us'] == {'lua': 30, 'native': 20}
    assert abs(result['addon_cpu_change_percent'] + 100 / 3) < 1e-10
    assert result['no_plugin_saved_us'] == 10 and result['original_saved_us'] == 15
    assert result['enabled_path_savings_minus_no_plugin_savings_us'] == 5
    assert result['controls']['lua']['original_minus_no_plugin_us'] == 20
    assert result['slope_cpu_change_percent'] == -50
    assert result['total_requests'] == 3240000 and not result['warnings']
    assert not result['pure_core_self_time_established']
    assert not result['core_caused_gc_separately_identified']
    assert not result['statistical_confidence_intervals_established']
    assert '3,240,000' in module.report(result)
    changed = copy.deepcopy(rows)
    next(r for r in changed if r['id'] == 'lua-loop-8')['cpu_us_per_request'] *= 2
    (p / 'results.json').write_text(json.dumps(changed))
    assert not module.analyze(p)['fits']['lua']['passes_linearity_diagnostics']
    (p / 'results.json').write_text(json.dumps(rows[:-1]))
    try:
        module.analyze(p)
    except AssertionError:
        pass
    else:
        raise AssertionError('incomplete matrix was accepted')
print('PASS exact linear arithmetic, decomposition, sensitivity, nonlinearity diagnostics, and incomplete-matrix rejection')

archive = args.results
env = json.loads((archive / 'environment.json').read_text())
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
assert sha(archive / 'harness/run.py') == env['runner_sha256']
assert sha(script.with_name('run.py')) == env['runner_sha256']
assert sha(archive / 'harness/load.py') == env['driver_sha256']
assert sha((script.with_name('load.py') if script.with_name('load.py').exists() else script.parent.parent / 'core_cost/load.py')) == env['driver_sha256']
assert sha(archive.parent / 'module.so') == env['original_module_sha256']
header_reference = json.loads((archive / 'nginx-before/upstream-echo.json').read_text())['headers']
for case in env['plan']:
    folder = archive / case['id']
    check = json.loads((folder / 'configuration-check.json').read_text())
    assert check['original_module_sha256'] == env['original_module_sha256']
    assert check['global_plugins'] == []
    assert not check['prometheus'] and not check['key_auth'] and not check['ai']
    assert check['enabled_plugins'] == ([] if case['kind'] in ['none', 'pure'] else ['limit-count'])
    assert check['validated_iterations_per_request'] == (case['k'] if case['kind'] == 'loop' else (1 if case['kind'] == 'original' else 0))
    assert json.loads((folder / 'upstream-echo.json').read_text())['headers'] == header_reference
    if case['kind'] == 'original':
        assert sha(folder / 'measured-limit-count.lua') == env['original_plugin_sha256']
    if case['mode'] != 'nginx':
        config = json.loads((folder / 'config.yaml').read_text())
        native = json.loads((folder / 'native.json').read_text())
        fixture = json.loads((folder / 'fixture.json').read_text())
        assert config['plugins'] == native['plugins'] == check['enabled_plugins']
        assert native['global_rules'] == fixture['global_rules'] == []
        assert native['consumers'] == fixture['consumers'] == []
        for route in native['routes']:
            expected = case['kind'] != 'none' and route['id'] == 'limit'
            assert set(route.get('plugins') or {}) == ({'limit-count'} if expected else set())
    row = json.loads((folder / 'result.json').read_text())
    assert abs(row['worker_cpu_seconds'] * 1e6 / row['requests'] - row['cpu_us_per_request']) < 1e-9
    assert not (folder / 'error.log').read_bytes()
restore = json.loads((archive / 'restoration.json').read_text())
assert restore['module_sha256'] == env['original_module_sha256']
assert restore['runtime_plugin_sha256'] == env['original_plugin_sha256']
assert restore['prometheus_port_closed'] and restore['timing_allocator_gc_probes_absent']
print('PASS all 18 configurations, quotas, identical upstream headers, original plugin and runner/driver/module hashes, CPU normalization, clean logs and restored demo')
