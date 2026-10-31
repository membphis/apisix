#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""No per-request timing probes: proxy baselines plus plugin amplification."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

FROZEN = Path('/tmp/apisix-v6-limit-only-frozen/experimental/native-http-core')
sys.path.insert(0, str(FROZEN / 'tools'))
sys.path.insert(0, str(FROZEN / 'tests'))
import demo
from benchmark import workers, usage
from test_demo import request

PURE = Path('/tmp/apisix-subtraction-nginx')
ORIGINAL = '/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so'
MODULE_SHA = '6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8'
PLUGIN_SOURCE = (FROZEN.parents[1] / 'apisix/plugins/limit-count.lua').read_text()
ORIGINAL_ACCESS = '''function _M.access(conf, ctx)
    return limit_count.rate_limit(conf, ctx, plugin_name, 1)
end'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wait_ready():
    for _ in range(100):
        try:
            if request('/plain/ready')[0] == 200:
                return
        except OSError:
            pass
        time.sleep(.05)
    raise AssertionError('gateway not ready')


def stop_all():
    demo.stop()
    demo.stop(PURE)


def start_pure(cpu):
    (PURE / 'conf').mkdir(parents=True, exist_ok=True)
    (PURE / 'logs').mkdir(exist_ok=True)
    text = f'''worker_processes 1;
worker_cpu_affinity {1 << cpu[0]:b};
error_log logs/error.log warn;
pid logs/nginx.pid;
events {{ worker_connections 10620; }}
http {{
    access_log off;
    keepalive_timeout 60s;
    server_tokens off;
    upstream backend {{
        server 127.0.0.1:1980;
        keepalive 320;
        keepalive_requests 1000;
        keepalive_timeout 60s;
    }}
    server {{
        listen 0.0.0.0:9080 default_server reuseport;
        more_set_headers "Server: APISIX/3.19.0";
        location / {{
            proxy_http_version 1.1;
            proxy_set_header Host $http_host;
            proxy_set_header Connection "";
            proxy_set_header X-Real-IP $remote_addr;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_set_header X-Forwarded-Proto $scheme;
            proxy_set_header X-Forwarded-Host $http_host;
            proxy_set_header X-Forwarded-Port $server_port;
            proxy_connect_timeout 100ms;
            proxy_read_timeout 100ms;
            proxy_send_timeout 100ms;
            proxy_next_upstream off;
            proxy_pass_header Date;
            proxy_pass http://backend;
        }}
    }}
}}
'''
    assert 'lua_' not in text and 'load_module' not in text
    (PURE / 'conf/nginx.conf').write_text(text)
    subprocess.run([demo.NGINX, '-p', str(PURE), '-c', 'conf/nginx.conf', '-t'], env=demo.NGINX_ENV, check=True)
    subprocess.run([demo.NGINX, '-p', str(PURE), '-c', 'conf/nginx.conf'], env=demo.NGINX_ENV, check=True)


def prepare(case, directory, cpu):
    stop_all()
    demo.NGINX_ENV.pop('LD_PRELOAD', None)
    demo.MODULE = ORIGINAL
    demo.backend(cpu[1])
    mode = case['mode']
    if mode == 'nginx':
        start_pure(cpu)
        prefix = PURE
        config = None
    else:
        fixture = json.loads((FROZEN / 'example.json').read_text())
        fixture['global_rules'] = []
        fixture['consumers'] = []
        for route in fixture['routes']:
            if route['id'] != 'limit' or case['kind'] == 'none':
                route['plugins'] = {}
        fixture_path = directory / 'fixture.json'
        fixture_path.write_text(json.dumps(fixture, indent=2))
        enabled = [] if case['kind'] == 'none' else ['limit-count']
        demo.prepare(mode, fixture=fixture_path, enabled_plugins=enabled, benchmark=True,
                     gateway_cpu=cpu[0], upstream_cpu=cpu[1])
        demo.stop()
        plugin = demo.ROOT / 'apisix/plugins/limit-count.lua'
        assert plugin.read_text() == PLUGIN_SOURCE
        if case['kind'] == 'loop':
            assert PLUGIN_SOURCE.count(ORIGINAL_ACCESS) == 1
            replacement = '''function _M.access(conf, ctx)
    for iteration = 1, __COUNT__ do
        local code, body = limit_count.rate_limit(conf, ctx, plugin_name, 1)
        if code then return code, body end
    end
end'''.replace('__COUNT__', str(case['k']))
            plugin.write_text(PLUGIN_SOURCE.replace(ORIGINAL_ACCESS, replacement, 1))
        else:
            assert digest(plugin) == hashlib.sha256(PLUGIN_SOURCE.encode()).hexdigest()
        config = json.loads((demo.ROOT / 'conf/config.yaml').read_text())
        native = json.loads((demo.ROOT / 'conf/native.json').read_text())
        assert config['plugins'] == native['plugins'] == enabled
        merged_plugins = native['local_conf']['plugins']
        assert merged_plugins == enabled or (not enabled and merged_plugins == {})
        assert config['plugin_attr']['prometheus']['enable_export_server'] is False
        path = demo.ROOT / 'conf/nginx.conf'
        text = path.read_text()
        assert 'core_cost_hooks' not in text and '__cost_' not in text
        marker = ('require("apisix.native.engine").init_worker()' if mode == 'native'
                  else 'apisix.http_init_worker()')
        audit = '''
        do
            local configured = require("apisix.core").config.local_conf().plugins
            assert(#configured == __N__)
            for _, name in ipairs(configured) do assert(name == "limit-count") end
            for name in pairs(package.loaded) do
                assert(not name:match("^apisix%.plugins%.ai") and not name:match("^apisix%.plugins%.key%-auth")
                       and not name:match("^apisix%.plugins%.prometheus"), "unexpected plugin: " .. name)
            end
        end
'''.replace('__N__', str(len(enabled)))
        assert text.count(marker) == 1
        text = text.replace(marker, marker + audit, 1)
        business = text.index('listen 0.0.0.0:9080')
        at = text.index('location / {', business)
        text = text[:at] + 'more_set_headers "Server: APISIX/3.19.0";\n        ' + text[at:]
        path.write_text(text)
        for entry in demo.ROOT.rglob('*'):
            if not entry.is_symlink(): entry.chmod(0o755 if entry.is_dir() else 0o644)
        subprocess.run([demo.NGINX, '-p', str(demo.ROOT), '-c', 'conf/nginx.conf', '-t'], env=demo.NGINX_ENV, check=True)
        subprocess.run([demo.NGINX, '-p', str(demo.ROOT), '-c', 'conf/nginx.conf'], env=demo.NGINX_ENV, check=True)
        prefix = demo.ROOT
        shutil.copy2(plugin, directory / 'measured-limit-count.lua')
    wait_ready()
    master = int((prefix / 'logs/nginx.pid').read_text())
    pids = workers(master)
    assert len(pids) == 1 and os.sched_getaffinity(pids[0]) == {cpu[0]}
    headers = []
    for _ in range(2):
        code, head, body = request('/limit/verify')
        assert code == 200 and body == b'ok\n'
        headers.append({k.lower(): v for k, v in head.items()})
    count = case.get('k', 0) if case['kind'] == 'loop' else (1 if case['kind'] == 'original' else 0)
    if count:
        assert int(headers[0]['x-ratelimit-remaining']) - int(headers[1]['x-ratelimit-remaining']) == count
    else:
        assert all('x-ratelimit-remaining' not in h for h in headers)
    code, _, body = request('/limit/echo', headers={'X-Demo-Echo': '1'})
    assert code == 200
    (directory / 'upstream-echo.json').write_text(body.decode())
    for name in ['nginx.conf', 'config.yaml', 'apisix.json', 'native.json']:
        path = prefix / 'conf' / name
        if path.exists(): shutil.copy2(path, directory / name)
    (directory / 'configuration-check.json').write_text(json.dumps({
        'case': case, 'enabled_plugins': config['plugins'] if config else [],
        'global_plugins': [], 'prometheus': False, 'key_auth': False, 'ai': False,
        'validated_iterations_per_request': count, 'response_headers': headers,
        'original_module_sha256': digest(ORIGINAL)}, indent=2))
    return prefix, pids


def backend_hits():
    code, _, body = request('/__hits', port=1980)
    assert code == 200
    return int(body)


def drive(rate, seconds, path, cpu):
    def pin(): os.sched_setaffinity(0, set(cpu[2:]))
    subprocess.run(['python3', '/tmp/subtraction-load.py', '--rate', str(rate), '--seconds', str(seconds),
                    '--connections', '32', '--output', str(path)], preexec_fn=pin, check=True)
    return json.loads(path.read_text())


def sample(case, directory, cpu, rate):
    prefix, pids = prepare(case, directory, cpu)
    drive(rate, 5, directory / 'warmup.json', cpu)
    time.sleep(.2)
    before_hits = backend_hits()
    offset = (prefix / 'logs/error.log').stat().st_size
    before = usage(pids)
    load = drive(rate, 60, directory / 'load.json', cpu)
    after = usage(pids)
    assert workers(int((prefix / 'logs/nginx.pid').read_text())) == pids
    assert backend_hits() - before_hits == load['completed'], 'more than one upstream request per external request'
    errors = (prefix / 'logs/error.log').read_bytes()[offset:]
    (directory / 'error.log').write_bytes(errors)
    assert b'[error]' not in errors and b'[alert]' not in errors
    row = {**case, 'duration_seconds': 60, 'requests': load['completed'], 'rate': rate,
           'worker_cpu_seconds': after['cpu_seconds'] - before['cpu_seconds'],
           'cpu_us_per_request': (after['cpu_seconds'] - before['cpu_seconds']) * 1e6 / load['completed'],
           'worker_cpu_utilization': (after['cpu_seconds'] - before['cpu_seconds']) / 60,
           'rss_end_bytes': after['rss_bytes'], 'load': load, 'error_rate': 0,
           'one_upstream_per_request': True, 'workers': pids}
    (directory / 'result.json').write_text(json.dumps(row, indent=2))
    print('RESULT ' + json.dumps(row), flush=True)
    return row


def cases():
    result = [{'id': 'nginx-before', 'mode': 'nginx', 'kind': 'pure'}]
    result += [{'id': mode + '-none-before', 'mode': mode, 'kind': 'none'} for mode in ['lua', 'native']]
    result += [{'id': mode + '-original', 'mode': mode, 'kind': 'original'} for mode in ['native', 'lua']]
    for index, k in enumerate([0, 1, 2, 4, 8]):
        for mode in (['lua', 'native'] if index % 2 == 0 else ['native', 'lua']):
            result.append({'id': f'{mode}-loop-{k}', 'mode': mode, 'kind': 'loop', 'k': k})
    result += [{'id': mode + '-none-after', 'mode': mode, 'kind': 'none'} for mode in ['native', 'lua']]
    result += [{'id': 'nginx-after', 'mode': 'nginx', 'kind': 'pure'}]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--rate', type=int, default=3000)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    assert digest(ORIGINAL) == MODULE_SHA
    assert 'LD_PRELOAD' not in os.environ
    demo.NGINX_ENV.pop('LD_PRELOAD', None)
    cpu = sorted(os.sched_getaffinity(0)); assert len(cpu) >= 5
    reference = json.loads((FROZEN / 'results/environment.json').read_text())
    for relative, expected in reference['native_sources_sha256'].items():
        assert digest(FROZEN / relative) == expected
    plan = cases()
    (args.out / 'environment.json').write_text(json.dumps({
        'started_utc': datetime.now(timezone.utc).isoformat(), 'source_commit': 'a3770f02b3adc2c295e993b8234da3e49f26864c',
        'original_module_sha256': MODULE_SHA, 'available_cpus': cpu, 'rate': args.rate,
        'sample_seconds': 60, 'warmup_seconds': 5, 'plan': plan,
        'runner_sha256': digest(__file__), 'driver_sha256': digest('/tmp/subtraction-load.py'),
        'original_plugin_sha256': hashlib.sha256(PLUGIN_SOURCE.encode()).hexdigest(),
        'clock_probes': False, 'allocator_probes': False, 'gc_probes': False, 'normal_gc': True}, indent=2))
    stop_all(); demo.stop(demo.UPSTREAM)
    rows = []
    try:
        if args.verify:
            for case in [plan[0], plan[1], plan[2], plan[3],
                         {'id': 'lua-loop-8', 'mode': 'lua', 'kind': 'loop', 'k': 8},
                         {'id': 'native-loop-8', 'mode': 'native', 'kind': 'loop', 'k': 8},
                         {'id': 'lua-loop-0', 'mode': 'lua', 'kind': 'loop', 'k': 0},
                         {'id': 'native-loop-0', 'mode': 'native', 'kind': 'loop', 'k': 0}]:
                directory = args.out / case['id']; directory.mkdir()
                _, pids = prepare(case, directory, cpu)
                if case['kind'] == 'loop' and case['k'] == 8:
                    before = usage(pids)
                    drive(args.rate, 5, directory / 'capacity-check.json', cpu)
                    utilization = (usage(pids)['cpu_seconds'] - before['cpu_seconds']) / 5
                    assert utilization < .70, ('reduce rate before formal matrix', utilization)
                    print('PASS slow-case headroom', case['id'], utilization, flush=True)
                print('PASS preflight', case['id'], flush=True)
        else:
            for index, case in enumerate(plan, 1):
                print(f'START {index}/{len(plan)} ' + case['id'], flush=True)
                directory = args.out / case['id']; directory.mkdir()
                rows.append(sample(case, directory, cpu, args.rate))
                (args.out / 'results.json').write_text(json.dumps(rows, indent=2))
    finally:
        stop_all()
        demo.prepare('native', fixture='/tmp/apisix-limit-only-v6-20261031T0644Z/fixture.json',
                     enabled_plugins=['limit-count'])
    assert digest(ORIGINAL) == MODULE_SHA
    print('PASS completed subtraction preflight' if args.verify else 'PASS all 18 subtraction samples', flush=True)


if __name__ == '__main__':
    main()
