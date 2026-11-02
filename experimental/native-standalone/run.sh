#!/usr/bin/env bash
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to You under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
set -euo pipefail
demo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd -- "$demo_dir/../.." && pwd)
demo_container=${NATIVE_DEMO_CONTAINER:-apisix-native-demo}
demo_image=${NATIVE_DEMO_IMAGE:-apisix-native-demo:local}

case "${1:-}" in
    build)
        docker build --secret "id=proxy_ca,src=${NATIVE_DEMO_CA:-/etc/ssl/certs/ca-certificates.crt}" \
            --build-arg "NATIVE_SANITIZE=${NATIVE_DEMO_SANITIZE:-off}" \
            -t "$demo_image" "$demo_dir"
        ;;
    start)
        if ! docker container inspect "$demo_container" >/dev/null 2>&1; then
            docker run -d --init --ulimit nofile=65536:65536 --name "$demo_container" \
                -p 127.0.0.1:9080:9080 -p 127.0.0.1:9091:9091 \
                --mount "type=bind,src=$source_dir,dst=/src,readonly" \
                --mount "type=bind,src=$demo_dir,dst=/demo,readonly" "$demo_image"
        fi
        docker exec "$demo_container" python3 /demo/prepare.py "${2:-native}" "${@:3}"
        ;;
    test)
        docker exec "$demo_container" python3 /demo/test_demo.py
        ;;
    bench)
        mkdir -p "$demo_dir/results"
        docker exec "$demo_container" python3 /demo/benchmark.py
        docker cp "$demo_container:/tmp/native-demo-results/." "$demo_dir/results/"
        ;;
    stop)
        docker stop "$demo_container"
        docker rm "$demo_container"
        ;;
    *)
        printf '%s\n' "Usage: $0 {build|start [native|lua] [--workers N]|test|bench|stop}" >&2
        exit 2
        ;;
esac
