# syntax=docker/dockerfile:1
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
FROM apache/apisix:3.14.1-debian@sha256:c228717165ecf4c0055818c9e2d9843f7b303f17a6df9e5fbaf8eafeb5007ae4
USER root
RUN --mount=type=secret,id=proxy_ca,required=true,mode=0444 \
    rm -f /etc/apt/sources.list.d/apisix.list && \
    printf '%s\n' \
      'deb https://snapshot.debian.org/archive/debian/20250929T000000Z bullseye main' \
      'deb https://snapshot.debian.org/archive/debian-security/20250929T000000Z bullseye-security main' \
      > /etc/apt/sources.list && \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca -o Acquire::Check-Valid-Until=false update && \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca install -y --no-install-recommends \
      gcc make libc6-dev libpcre3-dev libssl-dev zlib1g-dev curl python3 wrk procps binutils && \
    rm -rf /var/lib/apt/lists/* && \
    curl --fail --location --cacert /run/secrets/proxy_ca \
      https://openresty.org/download/openresty-1.27.1.2.tar.gz -o /tmp/openresty.tar.gz && \
    tar -xzf /tmp/openresty.tar.gz -C /tmp && rm /tmp/openresty.tar.gz
RUN --mount=type=secret,id=proxy_ca,required=true,mode=0444 \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca -o Acquire::Check-Valid-Until=false update && \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca install -y --no-install-recommends unzip cmake g++ && \
    rm -rf /var/lib/apt/lists/* && \
    curl --fail --location --cacert /run/secrets/proxy_ca \
      https://luarocks.org/releases/luarocks-3.11.1.tar.gz -o /tmp/luarocks.tar.gz && \
    tar -xzf /tmp/luarocks.tar.gz -C /tmp && \
    cd /tmp/luarocks-3.11.1 && \
    ./configure --with-lua=/usr/local/openresty/luajit --lua-version=5.1 \
      --with-lua-include=/usr/local/openresty/luajit/include/luajit-2.1 && \
    make install && rm -rf /tmp/luarocks*
RUN --mount=type=secret,id=proxy_ca,required=true,mode=0444 \
    for dependency in \
      api7-lua-resty-websocket-0.2.0-0 \
      api7-lua-resty-redis-connector-0.13.0-0 \
      nginx-lua-prometheus-api7-1.1.0-1 \
      api7-lua-rapidjson-0.7.2-0 \
      lua-resty-balancer-0.05-0 \
      api7-lua-resty-dns-client-7.1.2-0 \
      api7-lua-resty-http-0.2.4-0 \
      lua-resty-etcd-1.10.6-0 \
      lua-resty-healthcheck-api7-3.2.3-0 \
      lua-resty-redis-cluster-1.3.3-0 \
      jsonschema-0.9.13-0; do \
      curl --fail --location --cacert /run/secrets/proxy_ca \
        "https://luarocks.org/$dependency.src.rock" -o "/tmp/$dependency.src.rock" && \
      CURL_CA_BUNDLE=/run/secrets/proxy_ca luarocks --tree=/usr/local/apisix/deps \
        install --deps-mode=none "/tmp/$dependency.src.rock" && \
      rm "/tmp/$dependency.src.rock" || exit 1; \
    done
RUN --mount=type=secret,id=proxy_ca,required=true,mode=0444 \
    curl --fail --location --cacert /run/secrets/proxy_ca \
      https://github.com/api7/lua-resty-limit-traffic/archive/refs/tags/v1.2.0.tar.gz \
      -o /tmp/limit-traffic.tar.gz && \
    tar -xzf /tmp/limit-traffic.tar.gz -C /tmp && \
    cp -a /tmp/lua-resty-limit-traffic-1.2.0/lib/resty/limit/. \
      /usr/local/openresty/lualib/resty/limit/ && \
    rm -rf /tmp/limit-traffic.tar.gz /tmp/lua-resty-limit-traffic-1.2.0
COPY module /opt/native-module
ARG NATIVE_SANITIZE=off
RUN cd /tmp/openresty-1.27.1.2/bundle/nginx-1.27.1 && \
    demo_cc_flags=-O2 && demo_ld_flags='' && \
    if [ "$NATIVE_SANITIZE" = undefined ]; then \
      demo_cc_flags='-O2 -fsanitize=undefined -fno-sanitize-recover=undefined'; \
      demo_ld_flags='-fsanitize=undefined'; \
    elif [ "$NATIVE_SANITIZE" != off ]; then exit 1; fi && \
    ./configure --with-compat --with-http_ssl_module --with-stream --with-threads \
      --with-cc-opt="$demo_cc_flags" --with-ld-opt="$demo_ld_flags" \
      --add-dynamic-module=/opt/native-module && \
    make -j2 modules && mkdir -p /opt/native-module/lib && \
    cp objs/ngx_http_apisix_native_module.so /opt/native-module/lib/ && \
    rm -rf /tmp/openresty-1.27.1.2
ENTRYPOINT []
CMD ["sleep", "infinity"]
