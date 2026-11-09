// SPDX-License-Identifier: Apache-2.0
//! Upstream integration, ported from `module/upstream.c`
//! (`ac_upstream_native_directive`, `ac_native_peer_init`,
//! `ac_native_peer_get`, `ac_peer_free`). `apisix_rs_balancer` must precede
//! `keepalive` so the stock keepalive module wraps these callbacks.

use core::ffi::{c_char, c_void};
use core::ptr;

use ngx::ffi::*;

use crate::guard;
use crate::pipeline::RequestCtx;
use crate::vars;

pub(crate) unsafe extern "C" fn balancer_directive(
    cf: *mut ngx_conf_t,
    _cmd: *mut ngx_command_t,
    _conf: *mut c_void,
) -> *mut c_char {
    unsafe {
        let ctx = (*cf).ctx as *mut ngx_http_conf_ctx_t;
        let index = (*ptr::addr_of!(ngx_http_upstream_module)).ctx_index;
        let upstream = *(*ctx).srv_conf.add(index) as *mut ngx_http_upstream_srv_conf_t;
        if (*upstream).peer.init_upstream.is_some() {
            return c"apisix_rs_balancer must precede keepalive and other balancers".as_ptr() as *mut c_char;
        }
        (*upstream).peer.init_upstream = Some(init_upstream);
    }
    ptr::null_mut()
}

unsafe extern "C" fn init_upstream(_cf: *mut ngx_conf_t, upstream: *mut ngx_http_upstream_srv_conf_t) -> ngx_int_t {
    unsafe { (*upstream).peer.init = Some(init_peer) };
    NGX_OK as ngx_int_t
}

unsafe extern "C" fn init_peer(r: *mut ngx_http_request_t, _us: *mut ngx_http_upstream_srv_conf_t) -> ngx_int_t {
    let log = unsafe { (*(*r).connection).log };
    guard::contain(log, NGX_ERROR as ngx_int_t, "upstream peer init", || unsafe {
        let Some(ctx) = RequestCtx::get_mut(r) else { return NGX_ERROR as ngx_int_t };
        let Some(upstream) = ctx.state.route.and_then(|i| ctx.generation.core.routes[i].upstream.as_ref()) else {
            return NGX_ERROR as ngx_int_t;
        };
        if ctx.state.peer.is_none() {
            return NGX_ERROR as ngx_int_t;
        }
        let u = (*r).upstream;
        let conf = ngx_palloc((*r).pool, core::mem::size_of::<ngx_http_upstream_conf_t>()) as *mut ngx_http_upstream_conf_t;
        if conf.is_null() {
            return NGX_ERROR as ngx_int_t;
        }
        ptr::copy_nonoverlapping((*u).conf, conf, 1);
        (*u).conf = conf;
        (*conf).connect_timeout = upstream.connect_timeout_ms as ngx_msec_t;
        (*conf).send_timeout = upstream.send_timeout_ms as ngx_msec_t;
        (*conf).read_timeout = upstream.read_timeout_ms as ngx_msec_t;
        // The pinned runtime snapshots effective timeouts into upstream_t
        // before init_peer (OpenResty upstream_timeout_fields); update both.
        (*u).connect_timeout = upstream.connect_timeout_ms as ngx_msec_t;
        (*u).send_timeout = upstream.send_timeout_ms as ngx_msec_t;
        (*u).read_timeout = upstream.read_timeout_ms as ngx_msec_t;
        (*conf).next_upstream_timeout = upstream.retry_timeout_ms as ngx_msec_t;
        let mut retries = upstream.effective_retries() as ngx_uint_t;
        // Route retries never override `proxy_next_upstream off`.
        if (*conf).next_upstream & NGX_HTTP_UPSTREAM_FT_OFF as ngx_uint_t != 0 {
            retries = 0;
        }
        (*u).peer.data = ctx as *mut RequestCtx as *mut c_void;
        (*u).peer.get = Some(peer_get);
        (*u).peer.free = Some(peer_free);
        (*u).peer.tries = retries + 1;
        NGX_OK as ngx_int_t
    })
}

unsafe extern "C" fn peer_get(pc: *mut ngx_peer_connection_t, data: *mut c_void) -> ngx_int_t {
    let log = unsafe { (*pc).log };
    guard::contain(log, NGX_BUSY as ngx_int_t, "upstream peer get", || unsafe {
        let ctx = &mut *(data as *mut RequestCtx);
        let RequestCtx { generation, state, .. } = ctx;
        let Some(route) = state.route else { return NGX_BUSY as ngx_int_t };
        let Some(upstream) = generation.core.routes[route].upstream.as_ref() else { return NGX_BUSY as ngx_int_t };
        let Some(peer) = state.peer.as_mut() else { return NGX_BUSY as ngx_int_t };
        let now = ngx_current_msec as u64;
        if state.balance_attempts > 0
            && upstream.retry_timeout_ms > 0
            && now.wrapping_sub(peer.started_ms) > upstream.retry_timeout_ms
        {
            return NGX_BUSY as ngx_int_t;
        }
        // The first attempt uses the peer chosen during access.
        let selected = match peer.selected {
            Some(index) if state.balance_attempts == 0 => Some(index),
            _ => upstream.pick(peer, now),
        };
        let Some(index) = selected else { return NGX_BUSY as ngx_int_t };
        let address = &generation.peers[route][index];
        (*pc).sockaddr = address.sockaddr;
        (*pc).socklen = address.socklen;
        (*pc).name = &address.name as *const ngx_str_t as *mut ngx_str_t;
        (*pc).set_cached(0);
        (*pc).connection = ptr::null_mut();
        state.vars.set_bytes(vars::UPSTREAM_UNRESOLVED_HOST, &upstream.nodes[index].host);
        state.balance_attempts += 1;
        NGX_OK as ngx_int_t
    })
}

unsafe extern "C" fn peer_free(pc: *mut ngx_peer_connection_t, data: *mut c_void, flags: ngx_uint_t) {
    let log = unsafe { (*pc).log };
    guard::contain(log, (), "upstream peer free", || unsafe {
        let ctx = &mut *(data as *mut RequestCtx);
        let RequestCtx { generation, state, .. } = ctx;
        if let (Some(route), Some(peer)) = (state.route, state.peer.as_mut()) {
            if let Some(upstream) = generation.core.routes[route].upstream.as_ref() {
                upstream.release(peer, flags & NGX_PEER_FAILED as ngx_uint_t != 0, ngx_current_msec as u64);
            }
        }
        if (*pc).tries > 0 {
            (*pc).tries -= 1;
        }
    });
}
