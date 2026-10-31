#!/bin/sh
set -eu
cd /src/work/openresty-1.27.1.2/bundle/nginx-1.27.1
export LUAJIT_LIB=/usr/local/openresty/luajit/lib
export LUAJIT_INC=/usr/local/openresty/luajit/include/luajit-2.1
./configure \
  --prefix=/usr/local/openresty/nginx \
  --with-debug --with-threads --with-http_ssl_module --with-http_v2_module \
  --without-pcre2 --with-pcre-jit \
  --with-http_realip_module --with-http_stub_status_module \
  --with-stream --with-stream_ssl_module --with-stream_realip_module \
  --with-cc-opt='-O1 -g -DAPISIX_RUNTIME_VER=1.3.2 -I/usr/local/openresty/pcre/include -I/usr/local/openresty/zlib/include -I/usr/local/openresty/openssl3/include' \
  --with-ld-opt='-Wl,-rpath,/usr/local/openresty/luajit/lib:/usr/local/openresty/pcre/lib:/usr/local/openresty/zlib/lib:/usr/local/openresty/openssl3/lib -L/usr/local/openresty/pcre/lib -L/usr/local/openresty/zlib/lib -L/usr/local/openresty/openssl3/lib' \
  --add-module=../ngx_devel_kit-0.3.3 \
  --add-module=../ngx_lua-0.10.28 \
  --add-module=../ngx_stream_lua-0.0.16 \
  --add-module=../headers-more-nginx-module-0.37 \
  --add-module=/src/ngx_multi_upstream_module-1.3.2 \
  --add-module=/src/mod_dubbo-1.0.2 \
  --add-module=/src/apisix-nginx-module-1.19.2 \
  --add-module=/src/apisix-nginx-module-1.19.2/src/stream \
  --add-module=/src/apisix-nginx-module-1.19.2/src/meta \
  --add-module=/src/lua-var-nginx-module-0.5.3 \
  --add-module=/src/lua-resty-events-0.2.0
make -j4
