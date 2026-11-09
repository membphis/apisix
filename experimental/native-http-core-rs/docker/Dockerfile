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

# Adds a pinned Rust toolchain and libclang (for nginx-sys/bindgen) to the
# C research image. The Rust module is therefore compiled against the same
# configured NGINX source tree, compiler runtime and glibc as the C module.
FROM apisix-http-core:local
RUN --mount=type=secret,id=proxy_ca,required=true,mode=0444 \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca -o Acquire::Check-Valid-Until=false update && \
    apt-get -o Acquire::https::CaInfo=/run/secrets/proxy_ca install -y --no-install-recommends \
      clang libclang-dev && \
    rm -rf /var/lib/apt/lists/*
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
ARG RUST_TOOLCHAIN=1.97.0
RUN --mount=type=secret,id=proxy_ca,required=true,mode=0444 \
    curl --fail --location --cacert /run/secrets/proxy_ca \
      https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init -o /tmp/rustup-init && \
    chmod +x /tmp/rustup-init && \
    SSL_CERT_FILE=/run/secrets/proxy_ca /tmp/rustup-init -y --no-modify-path --profile minimal \
      --default-toolchain "$RUST_TOOLCHAIN" --component clippy && \
    rm /tmp/rustup-init && rustc --version && cargo --version && cargo clippy --version
