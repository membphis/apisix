#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Record an existing stable-Lua versus research-native forwarding difference."""
import hashlib
import json
import os
import test_demo as demo
from pathlib import Path

source = Path(os.environ['APISIX_CORE_SOURCE'])
records = []
try:
    for mode in ['lua', 'native']:
        demo.prepare(mode, single_plugin='prometheus')
        demo.ready('/missing', expected=404)
        path = '/metrics/a/../ok'
        status, _, body = demo.request(path, headers={'X-Demo-Echo':'1'})
        assert status == 200, (mode, status, body)
        forwarded = json.loads(body)['uri']
        expected = path if mode == 'lua' else '/metrics/ok'
        assert forwarded == expected, (mode, forwarded, expected)
        records.append({'mode':mode, 'input_uri':path, 'forwarded_uri':forwarded,
                        'status':status})
finally:
    demo.stop()
print(json.dumps({'source':str(source),
    'source_revision':os.environ.get('APISIX_CORE_SOURCE_REVISION'),
    'stable_init_sha256':hashlib.sha256((source/'apisix/init.lua').read_bytes()).hexdigest(),
    'records':records,
    'note':'Existing research-branch normalized forwarding policy is retained. '
           'Ordinary benchmark paths do not exercise this difference; not full-release equivalence.'}, indent=2))
