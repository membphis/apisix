// SPDX-License-Identifier: Apache-2.0
//! NGINX-independent logic of the Rust port of the APISIX v2 native key-auth
//! request pipeline (`experimental/native-http-core`, PR #1, "C 请求闭环").
//!
//! Everything in this crate is safe Rust and runs under `cargo test` on any
//! host. The `ngx_http_apisix_rs_module` crate owns all NGINX FFI and calls
//! into this crate for routing, consumer lookup, upstream peer selection and
//! the standalone configuration contract.
#![forbid(unsafe_code)]
#![cfg_attr(not(test), deny(clippy::unwrap_used, clippy::expect_used, clippy::panic))]

pub mod balancer;
pub mod config;
pub mod keyindex;
pub mod router;
pub mod uri;
