#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Container-side serial follow-up experiments. Never overwrites evidence."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import shutil

HERE = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get('APISIX_CORE_SWEEP_OUT', '/tmp/apisix-exhaustive-20261031'))
BASE = 'apisix_core_lazy_rate_json on;'
FLAGS = ['batch_vars', 'http_var_plan', 'lazy_route_opts', 'route_candidates',
         'request_pool', 'lazy_meta_matches', 'phase_index', 'phase_context',
         'request_header_flags', 'header_scratch', 'plain_uri', 'sdk_cache_replay',
         'native_var_writes', 'request_pool_native', 'matched_pool', 'balancer_peer_plan']
ALL = BASE + ' ' + ' '.join('apisix_core_' + name + ' on;' for name in FLAGS)
REV = '473043623c161966f3f6ef4e8f12d2d8667a12d2'

def run(label, script, args=(), directives=BASE, stable=False, extra=None):
    OUT.mkdir(exist_ok=True)
    env = os.environ.copy()
    env['APISIX_CORE_DIRECTIVES'] = directives
    env.pop('APISIX_CORE_SOURCE', None)
    if stable:
        env.update(APISIX_CORE_SOURCE='/stable', APISIX_CORE_SOURCE_REVISION=REV)
    if extra:
        env.update(extra)
    log = OUT / (label + '.log')
    with log.open('x') as stream:
        print('START', label, flush=True)
        result = subprocess.run(['python3', str(HERE / script), *args], env=env,
                                stdout=stream, stderr=subprocess.STDOUT)
    with (OUT / 'events.jsonl').open('a') as stream:
        stream.write(json.dumps({'label':label, 'exit_code':result.returncode,
                                 'time_unix':time.time()}) + '\n')
    result.check_returncode()
    print('DONE', label, flush=True)

def rebuild(label, optimization='O2', sanitizer=''):
    run(label, 'tools/rebuild.py', extra={'APISIX_CORE_OPTIMIZATION':optimization,
                                        'APISIX_CORE_SANITIZER':sanitizer})
    for source, suffix in [('/tmp/apisix-core-compile.log', '-compile.log'),
                           ('/opt/apisix-core-module/lib/build.json', '-build.json')]:
        shutil.copyfile(source, OUT / (label + suffix))

def bench(label, prefix, directives=BASE, modes=('native',)):
    run(label, 'tools/benchmark.py', ['--suite','single','--prefixes',*prefix,
        '--modes',*modes,'--variant',label,'--out',str(OUT/label)], directives, True)

def profile(label, mode, prefix, sampler, directives=BASE, extra=None):
    run(label, 'tools/profile.py', [mode, '--suite', 'isolated-single',
        '--prefix', prefix, '--sampler', sampler], directives, True, extra)
    source = Path('/tmp/apisix-core-profile-'+mode+'-isolated-single-'+prefix+'-'+sampler+'.txt')
    for suffix in ['', '.wrk.txt', '.nginx.conf', '.metadata.json', '.functions.json', '.maps']:
        path = Path(str(source)+suffix)
        if path.exists():
            shutil.copyfile(path, OUT/(label+suffix+'.raw'))

def verify(tag, sanitizer=''):
    rebuild(tag+'-build', sanitizer=sanitizer)
    extra = {'APISIX_CORE_SANITIZER':sanitizer}
    for profile in ['default','servlet','tail','encoded','trusted','untrusted']:
        run(tag+'-core-'+profile,'tests/test_core.py',directives=ALL,
            extra={**extra,'APISIX_CORE_PROFILE':profile})
    run(tag+'-demo','tests/test_demo.py',directives=ALL,extra=extra)
    for suite in ['http_plans','route_allocations','phase_context','request_pool','header_fast',
                  'sdk_false','matched_pool','peer_plan']:
        run(tag+'-'+suite,'tests/test_'+suite+'.py',directives=ALL,
            stable=(suite == 'sdk_false'),extra=extra)
    run(tag+'-single-on','tests/test_single.py',directives=ALL,stable=True,extra=extra)
    run(tag+'-single-off','tests/test_single.py',directives=BASE,stable=True,extra=extra)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['verify','sanitize','route','screen','extras','compilers','linkage','final','diagnostics'])
    parser.add_argument('--directives', default=BASE)
    parser.add_argument('--label', default='final')
    parser.add_argument('--reverse', action='store_true', help='reverse paired mode order')
    args = parser.parse_args()
    if args.action == 'verify':
        verify('verify-o2')
    elif args.action == 'sanitize':
        verify('verify-asan','address,undefined')
        rebuild('restore-o2')
    elif args.action == 'route':
        for prefix in ['metrics','auth']:
            label = 'route-profile-'+prefix
            run(label,'tools/profile.py',['native','--suite','isolated-single',
                '--prefix',prefix,'--sampler','route'],stable=True)
            source = Path('/tmp/apisix-core-profile-native-isolated-single-'+prefix+'-route.txt')
            for suffix in ['', '.wrk.txt', '.nginx.conf', '.metadata.json']:
                shutil.copyfile(str(source)+suffix, OUT/(label+suffix+'.raw'))
        rebuild('route-restore-o2')
    elif args.action == 'screen':
        groups = [('metrics', ['batch_vars','http_var_plan','lazy_route_opts',
                               'route_candidates','request_pool','lazy_meta_matches',
                               'phase_index','phase_context','plain_uri','native_var_writes']),
                  ('auth', ['request_header_flags','header_scratch','sdk_cache_replay'])]
        for prefix, flags in groups:
            bench('control-'+prefix+'-0', [prefix])
            for i, flag in enumerate(flags):
                bench('screen-'+flag,[prefix],BASE+' apisix_core_'+flag+' on;')
                if (i+1)%3 == 0 or i+1==len(flags):
                    bench('control-'+prefix+'-'+str(i+1),[prefix])
    elif args.action == 'extras':
        bench('extra-control-0',['metrics'])
        bench('extra-request_pool_native',['metrics'],BASE+' apisix_core_request_pool_native on;')
        bench('extra-balancer_peer_plan',['metrics'],BASE+' apisix_core_balancer_peer_plan on;')
        bench('extra-control-2',['metrics'])
        matched = BASE+' apisix_core_lazy_route_opts on;'
        bench('extra-matched-control-0',['metrics'],matched)
        bench('extra-matched_pool',['metrics'],matched+' apisix_core_matched_pool on;')
        bench('extra-matched-control-1',['metrics'],matched)
    elif args.action == 'compilers':
        for optimization in ['O3','O3-lto']:
            rebuild('build-'+optimization,optimization)
            run('compiler-check-'+optimization,'tests/test_single.py',stable=True)
            bench('compiler-'+optimization,['metrics'])
        rebuild('compiler-restore-o2')
        bench('compiler-control-o2',['metrics'])
    elif args.action == 'linkage':
        rebuild('build-O3-lto-local', 'O3-lto-local')
        for profile_name in ['default','servlet','tail','encoded','trusted','untrusted']:
            run('compiler-local-core-'+profile_name, 'tests/test_core.py', directives=ALL,
                extra={'APISIX_CORE_PROFILE': profile_name})
        run('compiler-check-O3-lto-local', 'tests/test_single.py', stable=True)
        bench('compiler-O3-lto-local', ['metrics'])
        rebuild('compiler-local-restore-o2')
        bench('compiler-local-control-o2', ['metrics'])
    elif args.action == 'diagnostics':
        optimization = json.loads(Path('/opt/apisix-core-module/lib/build.json').read_text())['optimization']
        # Validate the selected bundle itself on the same binary. Exploratory
        # single-flag improvements cannot be added together.
        bench('combo-control-0', ['metrics'])
        bench('combo-selected', ['metrics'], args.directives)
        bench('combo-control-1', ['metrics'])
        profile('alloc-base', 'native', 'metrics', 'alloc')
        profile('alloc-selected', 'native', 'metrics', 'alloc', args.directives)
        profile('alloc-lua', 'lua', 'metrics', 'alloc')
        for prefix in ['metrics', 'auth', 'limit']:
            profile('jit-selected-'+prefix, 'native', prefix, 'jit', args.directives)
        profile('traces-selected-metrics', 'native', 'metrics', 'traces', args.directives)
        profile('pc-selected-metrics', 'native', 'metrics', 'pc', args.directives)
        profile('functions-selected-metrics', 'native', 'metrics', 'functions', args.directives,
                {'APISIX_CORE_OPTIMIZATION': optimization})
        rebuild('post-profile-restore', optimization)
        run('uri-policy-difference', 'tests/check_uri_policy.py',
            directives=args.directives, stable=True)
    else:
        bench(args.label,['metrics','auth','limit'],args.directives,
              ('native','lua') if args.reverse else ('lua','native'))
