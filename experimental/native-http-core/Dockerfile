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
FROM apisix-native-demo:local
RUN --mount=type=secret,id=proxy_ca,required=true,mode=0444 \
    curl --fail --location --cacert /run/secrets/proxy_ca \
      https://openresty.org/download/openresty-1.27.1.2.tar.gz -o /tmp/openresty.tar.gz && \
    tar -xzf /tmp/openresty.tar.gz -C /tmp && rm /tmp/openresty.tar.gz
COPY module /opt/apisix-core-module
RUN cd /tmp/openresty-1.27.1.2/bundle/nginx-1.27.1 && \
    ./configure --with-compat --with-http_ssl_module --with-stream --with-threads \
      --with-cc-opt='-O2 -I../ngx_lua-0.10.28/src/api -I/usr/local/openresty/luajit/include/luajit-2.1' \
      --add-dynamic-module=/opt/apisix-core-module && \
    make -j2 modules && mkdir -p /opt/apisix-core-module/lib && \
    cp objs/ngx_http_apisix_core_module.so /opt/apisix-core-module/lib/
