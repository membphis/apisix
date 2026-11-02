#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Container-side scoped-core diagnostic on the frozen limit-only implementation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

FROZEN = Path('/tmp/apisix-v6-limit-only-frozen/experimental/native-http-core')
sys.path.insert(0, str(FROZEN / 'tools'))
sys.path.insert(0, str(FROZEN / 'tests'))
import demo
from benchmark import workers, usage
from test_demo import ready, request

ORIGINAL_MODULE = '/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so'
DIAGNOSTIC_MODULE = '/tmp/native-core-cost.so'
FIXTURE = Path('/tmp/apisix-limit-only-v6-20261031T0644Z/fixture.json')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['lua', 'native'])
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--timing-only', action='store_true')
    parser.add_argument('--gc-only', action='store_true')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    assert not (args.gc_only and args.profile)
    assert not args.timing_only or args.profile
    instrumented = args.profile or args.gc_only
    args.out.mkdir(parents=True, exist_ok=False)
    args.out.chmod(0o777)
    artifacts = ['/tmp/core-cost-run.py', '/tmp/core-cost-load.py', '/tmp/core_cost_hooks.lua',
                 '/tmp/core-cost-sampler.so', ORIGINAL_MODULE, DIAGNOSTIC_MODULE]
    artifact_hashes = {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in artifacts}
    (args.out / 'artifacts.json').write_text(json.dumps(artifact_hashes, indent=2))
    cpu = sorted(os.sched_getaffinity(0))
    # nginx -s quit parses the previous configuration too. It may reference the
    # diagnostic module even when the next run is an uninstrumented control.
    demo.NGINX_ENV['LD_PRELOAD'] = '/tmp/core-cost-sampler.so'
    demo.stop()
    demo.stop(demo.UPSTREAM)
    demo.MODULE = DIAGNOSTIC_MODULE if args.profile and args.mode == 'native' else ORIGINAL_MODULE
    if instrumented:
        demo.NGINX_ENV['LD_PRELOAD'] = '/tmp/core-cost-sampler.so'
    else:
        demo.NGINX_ENV.pop('LD_PRELOAD', None)
    demo.prepare(args.mode, fixture=FIXTURE, enabled_plugins=['limit-count'],
                 benchmark=True, gateway_cpu=cpu[0], upstream_cpu=cpu[1])
    ready()
    if instrumented:
        demo.stop()
        path = demo.ROOT / 'conf/nginx.conf'
        text = path.read_text()
        injection = ('require "resty.core"\n'
                     '        package.path = "/tmp/?.lua;" .. package.path\n'
                     '        require("core_cost_hooks")' + ('.prepare()' if args.profile else ''))
        assert text.count('require "resty.core"') == 1
        text = text.replace('require "resty.core"', injection, 1)
        marker = ('require("apisix.native.engine").init_worker()' if args.mode == 'native'
                  else 'apisix.http_init_worker()')
        assert text.count(marker) == 1
        if args.profile:
            text = text.replace(marker, marker + '\n        require("core_cost_hooks").install("' + args.mode + '", ' + ('true' if args.verify else 'false') + ')', 1)
        control = '''
        location = /__cost_start { content_by_lua_block {
            assert(require("core_cost_hooks").sampler.start(__STRIDE__, __ALLOC__, __GC__)); ngx.say("ok")
        } }
        location = /__cost_reset { content_by_lua_block {
            assert(require("core_cost_hooks").sampler.reset()); ngx.say("ok")
        } }
        location = /__cost_stop { content_by_lua_block {
            assert(require("core_cost_hooks").sampler.stop("__OUTPUT__")); ngx.say("ok")
        } }
'''.replace('__OUTPUT__', str(args.out / 'scope.json')).replace('__STRIDE__', '1' if args.verify else '64').replace('__ALLOC__', 'false' if args.timing_only or args.gc_only else 'true').replace('__GC__', 'false' if args.timing_only else 'true')
        business = text.index('listen 0.0.0.0:9080')
        at = text.index('location / {', business)
        text = text[:at] + control + '\n        ' + text[at:]
        path.write_text(text)
        subprocess.run([demo.NGINX, '-p', str(demo.ROOT), '-c', 'conf/nginx.conf', '-t'], env=demo.NGINX_ENV, check=True)
        subprocess.run([demo.NGINX, '-p', str(demo.ROOT), '-c', 'conf/nginx.conf'], env=demo.NGINX_ENV, check=True)
        ready()
    config = json.loads((demo.ROOT / 'conf/config.yaml').read_text())
    resources = json.loads((demo.ROOT / 'conf/apisix.json').read_text())
    assert config['plugins'] == ['limit-count'] and resources['global_rules'] == []
    for name in ['nginx.conf', 'config.yaml', 'apisix.json', 'native.json']:
        shutil.copy2(demo.ROOT / 'conf' / name, args.out / name)
    pids = workers()
    assert len(pids) == 1 and os.sched_getaffinity(pids[0]) == {cpu[0]}
    code, headers, body = request('/limit/verify')
    assert code == 200 and body == b'ok\n' and 'X-RateLimit-Limit' in headers
    def control(path):
        code, _, body = request(path)
        assert code == 200 and body == b'ok\n', (path, code, body)
    def drive(seconds, name):
        def pin(): os.sched_setaffinity(0, set(cpu[2:]))
        subprocess.run(['python3', '/tmp/core-cost-load.py', '--rate', '8000', '--seconds', str(seconds),
                        '--connections', '32', '--output', str(args.out / name)], preexec_fn=pin, check=True)
    if instrumented:
        control('/__cost_start')
    if args.verify:
        for _ in range(160):
            assert request('/limit/verify')[0] == 200
        if args.profile:
            control('/__cost_stop')
            scoped = json.loads((args.out / 'scope.json').read_text())
            assert scoped['invalid'] == scoped['missing'] == scoped['pending'] == 0, scoped
            assert scoped['completed'] == 160 and scoped['plugin_entries'] == 160, scoped
            assert scoped['sdk_entries'] > scoped['completed'], scoped
            assert scoped['sampled_completed'] > 0 and scoped['allocator_restored'], scoped
            shutil.copy2(args.out / 'scope.json', args.out / 'boundary-counts.json')
            probes = []
            for query, field, minimum in [('sleep=0.03', 'wait', .025),
                                           ('cost_plugin_ns=2000000', 'plugin', 1500000),
                                           ('cost_sdk_ns=2000000', 'core', 1500000)]:
                control('/__cost_start')
                elapsed = []
                for _ in range(10):
                    started = time.perf_counter()
                    assert request('/limit/verify?' + query)[0] == 200
                    elapsed.append(time.perf_counter() - started)
                control('/__cost_stop')
                value = json.loads((args.out / 'scope.json').read_text())
                assert value['invalid'] == value['missing'] == value['pending'] == 0, value
                if field == 'wait':
                    assert min(elapsed) > minimum and value['core_active_wall']['p99_ns'] < 10000000, value
                else:
                    assert value[field + '_cpu']['mean_ns'] > minimum, value
                    other = 'core' if field == 'plugin' else 'plugin'
                    assert value[other + '_cpu']['mean_ns'] < 1000000, value
                probes.append({'query': query, 'wire_seconds': elapsed, 'scope': value})
            (args.out / 'attribution-probes.json').write_text(json.dumps(probes, indent=2))
        print('PASS scoped preflight', args.mode, args.profile, flush=True)
        return
    drive(5, 'warmup.json')
    time.sleep(.2)
    if instrumented:
        control('/__cost_reset')
    offset = (demo.ROOT / 'logs/error.log').stat().st_size
    before = usage(pids)
    drive(60, 'load.json')
    after = usage(pids)
    time.sleep(.2)
    assert workers() == pids
    if instrumented:
        control('/__cost_stop')
    errors = (demo.ROOT / 'logs/error.log').read_bytes()[offset:]
    (args.out / 'error.log').write_bytes(errors)
    assert b'[error]' not in errors and b'[alert]' not in errors, errors[:2000]
    load = json.loads((args.out / 'load.json').read_text())
    summary = {'mode': args.mode, 'profile': instrumented, 'scope_clocks': args.profile,
               'timing_only': args.timing_only, 'gc_only': args.gc_only, 'rate': 8000,
               'duration_seconds': 60, 'workers': pids, 'requests': load['completed'],
               'worker_cpu_seconds': after['cpu_seconds'] - before['cpu_seconds'],
               'worker_cpu_us_per_request': (after['cpu_seconds'] - before['cpu_seconds']) * 1e6 / load['completed'],
               'module_sha256': hashlib.sha256(Path(demo.MODULE).read_bytes()).hexdigest(),
               'original_module_sha256': hashlib.sha256(Path(ORIGINAL_MODULE).read_bytes()).hexdigest(),
               'source_commit': 'a3770f02b3adc2c295e993b8234da3e49f26864c',
               'artifact_sha256': artifact_hashes,
               'load': load}
    if args.profile:
        scoped = json.loads((args.out / 'scope.json').read_text())
        assert scoped['invalid'] == scoped['missing'] == scoped['pending'] == 0, scoped
        assert abs(scoped['completed'] - load['completed']) <= 32
        assert scoped['plugin_entries'] == scoped['completed']
        assert scoped['sdk_entries'] > scoped['completed']
        assert scoped['sampled_completed'] > 1000 and scoped['allocator_restored']
        summary['scope'] = scoped
    elif args.gc_only:
        scoped = json.loads((args.out / 'scope.json').read_text())
        assert scoped['invalid'] == scoped['missing'] == scoped['pending'] == 0
        assert scoped['completed'] == 0 and scoped['sampled_completed'] == 0
        assert not scoped['allocator_enabled'] and scoped['gc_sampling_enabled']
        assert scoped['gc_cpu_samples'] > 0
        summary['scope'] = scoped
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    assert artifact_hashes == {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in artifacts}
    print('PASS 60-second diagnostic', args.mode, args.profile, flush=True)


if __name__ == '__main__':
    main()
