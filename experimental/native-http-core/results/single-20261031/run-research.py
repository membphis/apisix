import subprocess, os, pathlib, sys
ROOT=pathlib.Path('/workspace/apisix')
OUT=ROOT/'experimental/native-http-core/results/single-20261031'
NAME='apisix-http-core-dev'
BASE='/src/experimental/native-http-core'
REV='473043623c161966f3f6ef4e8f12d2d8667a12d2'
ALL='apisix_core_log_var_plan on; apisix_core_lazy_rate_json on; apisix_core_fused_balancer on;'
env=os.environ.copy()
for k in ('DOCKER_HOST','DOCKER_CONTEXT','DOCKER_TLS','DOCKER_TLS_VERIFY','DOCKER_CERT_PATH'): env.pop(k,None)
DOCKER=['docker','--host=unix:///var/run/docker.sock']
def run(name, args, directives='', stable=True):
    command=DOCKER+['exec','-e','APISIX_CORE_DIRECTIVES='+directives]
    if stable: command+=['-e','APISIX_CORE_SOURCE=/stable','-e','APISIX_CORE_SOURCE_REVISION='+REV]
    command += [NAME,'python3',BASE+'/'+args[0]]+args[1:]
    print('START',name,flush=True)
    with (OUT/(name+'.log')).open('w') as out:
        subprocess.run(command,env=env,stdout=out,stderr=subprocess.STDOUT,check=True)
    print('DONE',name,flush=True)
def copy(src,dst): subprocess.run(DOCKER+['cp',NAME+':'+src,str(OUT/dst)],env=env,check=True)
def bench(name,prefix,flags='',modes=['native']):
    run(name,['tools/benchmark.py','--suite','single','--prefixes']+prefix+['--modes']+modes+['--variant',name,'--out','/tmp/native-single/'+name],flags)
    copy('/tmp/native-single/'+name,name)
if sys.argv[1]=='focused':
    run('verify-log-plan',['tests/test_log_var_plan.py'],stable=False)
    run('verify-single-off',['tests/test_single.py'])
    run('verify-single-on',['tests/test_single.py'],ALL)
elif sys.argv[1]=='profiles':
    for sampler,modes,prefixes in [('jit',['lua','native'],['prometheus','limit']),('traces',['native'],['prometheus','limit']),('functions',['native'],['prometheus','limit'])]:
        for mode in modes:
            for prefix in prefixes:
                label=f'profile-{mode}-{prefix}-{sampler}'
                run(label,['tools/profile.py',mode,'--suite','isolated-single','--prefix',prefix,'--sampler',sampler])
                filename=f'/tmp/apisix-core-profile-{mode}-isolated-single-{prefix}-{sampler}.txt'
                for suffix in ['', '.wrk.txt','.nginx.conf','.metadata.json']+(['.functions.json'] if sampler=='functions' else []):
                    copy(filename+suffix,label+suffix+'.raw')
    run('restore-optimized',['tools/rebuild.py'],stable=False)
elif sys.argv[1]=='ablations':
    bench('var-off',['metrics'])
    bench('var-on',['metrics'],'apisix_core_log_var_plan on;')
    bench('balance-on',['auth'],'apisix_core_fused_balancer on;')
    bench('balance-off',['auth'])
    bench('json-off',['limit'])
    bench('json-on',['limit'],'apisix_core_lazy_rate_json on;')
elif sys.argv[1]=='final':
    bench('final',['metrics','auth','limit'],sys.argv[2] if len(sys.argv)>2 else ALL,['lua','native'])
