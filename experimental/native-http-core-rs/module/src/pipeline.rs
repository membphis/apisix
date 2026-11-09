// SPDX-License-Identifier: Apache-2.0
//! Access and log phase handlers: the Rust counterpart of `ac_pipeline_access`
//! and `ac_pipeline_log` in `module/pipeline.c`.
//!
//! access: request preparation -> routing -> key-auth -> upstream preparation;
//! success returns `NGX_DECLINED` so `proxy_pass` runs, rejections send the
//! same JSON bodies as the C pipeline.

use core::ffi::c_void;
use core::ptr;
use std::rc::Rc;

use apisix_rs_core::balancer::Peer;
use apisix_rs_core::router::host_rank;
use apisix_rs_core::uri::only_decoded_or_merged;
use ngx::core::Pool;
use ngx::ffi::*;
use ngx::http::HttpModuleConfExt;

use crate::runtime::{self, GenRt};
use crate::vars::{self, Values};
use crate::{guard, http, keyauth, module, LocConf};

const NOT_FOUND: &[u8] = b"{\"error_msg\":\"404 Route Not Found\"}\n";
const SERVER_ERROR: ngx_int_t = NGX_HTTP_INTERNAL_SERVER_ERROR as ngx_int_t;
const OK: ngx_int_t = NGX_OK as ngx_int_t;

/// Mutable per-request state, separate from the pinned generation so both can
/// be borrowed at once.
pub(crate) struct State {
    pub(crate) route: Option<usize>,
    pub(crate) consumer: Option<usize>,
    pub(crate) peer: Option<Peer>,
    pub(crate) balance_attempts: u32,
    pub(crate) vars: Values,
    pub(crate) proxied: bool,
    pub(crate) logged: bool,
}

impl State {
    pub(crate) fn new() -> State {
        State {
            route: None,
            consumer: None,
            peer: None,
            balance_attempts: 0,
            vars: Values::new(),
            proxied: false,
            logged: false,
        }
    }
}

/// Request context, allocated in the request pool; dropping it (pool
/// cleanup) releases the pinned generation.
pub(crate) struct RequestCtx {
    pub(crate) generation: Rc<GenRt>,
    pub(crate) state: State,
    /// Step 1 (Lua plugins on the Rust core) only; `None` in the pipeline.
    pub(crate) lua: Option<crate::step1::LuaRequest>,
}

impl Drop for RequestCtx {
    fn drop(&mut self) {
        if self.lua.is_some() {
            crate::step1::cleanup(self);
        }
    }
}

impl RequestCtx {
    #[inline]
    pub(crate) unsafe fn get<'a>(r: *mut ngx_http_request_t) -> Option<&'a RequestCtx> {
        unsafe { (*(*r).ctx.add(module().ctx_index) as *const RequestCtx).as_ref() }
    }

    #[inline]
    pub(crate) unsafe fn get_mut<'a>(r: *mut ngx_http_request_t) -> Option<&'a mut RequestCtx> {
        unsafe { (*(*r).ctx.add(module().ctx_index) as *mut RequestCtx).as_mut() }
    }
}

pub(crate) unsafe extern "C" fn access_handler(r: *mut ngx_http_request_t) -> ngx_int_t {
    let log = unsafe { (*(*r).connection).log };
    guard::contain(log, SERVER_ERROR, "access handler", || unsafe { access(r) })
}

unsafe fn access(r: *mut ngx_http_request_t) -> ngx_int_t {
    let enabled = unsafe { (*r).http_location_conf_unchecked::<LocConf>(module()) }
        .is_some_and(|conf| unsafe { (*conf.as_ptr()).proxy } == 1);
    if !enabled || unsafe { RequestCtx::get(r) }.is_some() {
        return NGX_DECLINED as ngx_int_t;
    }
    let Some(generation) = runtime::current() else {
        return NGX_HTTP_SERVICE_UNAVAILABLE as ngx_int_t;
    };
    #[cfg(feature = "panic-test")]
    if unsafe { http::bytes(&(*r).uri) } == b"/__apisix_rs_panic" {
        panic!("intentional panic from the panic-test build");
    }
    let pool = unsafe { Pool::from_ngx_pool((*r).pool) };
    let ctx = pool.allocate(RequestCtx { generation, state: State::new(), lua: None });
    if ctx.is_null() {
        return SERVER_ERROR;
    }
    unsafe { *(*r).ctx.add(module().ctx_index) = ctx as *mut c_void };
    let RequestCtx { generation, state, .. } = unsafe { &mut *ctx };
    let generation: &GenRt = generation;

    let mut body: &'static [u8] = &[];
    let mut rc = unsafe { prepare(r, generation, state) };
    if rc == OK {
        rc = unsafe { route(r, generation, state) };
        if rc == NGX_HTTP_NOT_FOUND as ngx_int_t {
            body = NOT_FOUND;
        }
    }
    if rc == OK {
        if let Some(conf) = state.route.and_then(|index| generation.core.routes[index].key_auth.as_ref()) {
            rc = unsafe { keyauth::run(r, generation, conf, state, &mut body) };
        }
    }
    if rc == OK {
        rc = unsafe { upstream(r, generation, state) };
    }
    if rc == OK {
        state.proxied = true;
        return NGX_DECLINED as ngx_int_t;
    }
    if body.is_empty() {
        return rc;
    }
    unsafe { http::send_json(r, rc, body) }
}

const XFF: (&[u8], ngx_uint_t) = (b"http_x_forwarded_for", http::hash(b"http_x_forwarded_for"));
const ORIGINALS: [(&[u8], ngx_uint_t); 4] = [
    (b"http_x_forwarded_proto", http::hash(b"http_x_forwarded_proto")),
    (b"http_x_forwarded_host", http::hash(b"http_x_forwarded_host")),
    (b"http_x_forwarded_port", http::hash(b"http_x_forwarded_port")),
    (b"http_forwarded", http::hash(b"http_forwarded")),
];
const FORWARDED_NAMES: [&[u8]; 4] = [b"X-Forwarded-Proto", b"X-Forwarded-Host", b"X-Forwarded-Port", b"Forwarded"];

/// `ac_http_prepare_native`: URI policy and sanitized forwarding headers.
unsafe fn prepare(r: *mut ngx_http_request_t, _generation: &GenRt, state: &mut State) -> ngx_int_t {
    unsafe {
        let unparsed = http::bytes(&(*r).unparsed_uri);
        let path = match unparsed.iter().position(|&b| b == b'?') {
            Some(query) => &unparsed[..query],
            None => unparsed,
        };
        let Some(scratch) = http::pool_bytes(r, path.len() + 1) else { return SERVER_ERROR };
        if !only_decoded_or_merged(path, http::bytes(&(*r).uri), scratch) {
            (*r).set_valid_unparsed_uri(0);
        }
        // The routing URI (delete_uri_tail_slash) is derived in `route`.
        state.vars.set(vars::REQUEST_LINE, (*r).request_line);
        if let Some(value) = http::variable(r, XFF.0, XFF.1) {
            state.vars.set_bytes(vars::ORIGINAL_X_FORWARDED_FOR, value);
        }
        let host_header = (*r).headers_in.host;
        let host = if host_header.is_null() { http::bytes(&(*r).headers_in.server) } else { http::bytes(&(*host_header).value) };
        let mut port = http::variable(r, b"server_port", http::hash(b"server_port"));
        if let Some(colon) = host.iter().rposition(|&b| b == b':') {
            let digits = &host[colon + 1..];
            if !digits.is_empty() && digits.iter().all(u8::is_ascii_digit) {
                port = Some(digits);
            }
        }
        let scheme = http::variable(r, b"scheme", http::hash(b"scheme"));
        let values = [scheme, Some(host), port, None];
        for (index, value) in values.into_iter().enumerate() {
            // Originals are still exposed through the same variable names.
            let original = http::variable(r, ORIGINALS[index].0, ORIGINALS[index].1).unwrap_or_default();
            state.vars.set_bytes(vars::ORIGINAL_FORWARDING + index, original);
            if !http::set_request_header(r, FORWARDED_NAMES[index], value) {
                return SERVER_ERROR;
            }
        }
    }
    OK
}

/// `ac_match_native`: compressed tree walk with host/method/CIDR predicates.
unsafe fn route(r: *mut ngx_http_request_t, generation: &GenRt, state: &mut State) -> ngx_int_t {
    let core = &generation.core;
    let (server, host_header, method, sockaddr) = unsafe {
        let host = (*r).headers_in.host;
        (
            http::bytes(&(*r).headers_in.server),
            if host.is_null() { None } else { Some(http::bytes(&(*host).value)) },
            (*r).method as u32,
            (*(*r).connection).sockaddr,
        )
    };
    let mut uri = unsafe { http::bytes(&(*r).uri) };
    if core.settings.delete_tail_slash && uri.last() == Some(&b'/') {
        uri = &uri[..uri.len() - 1];
    }
    let host_router = core.settings.host_router;
    let found = core.router.find(uri, |index| {
        let candidate = &core.routes[index];
        let rank = host_rank(&candidate.hosts, host_router, server, host_header)?;
        if candidate.methods != 0 && candidate.methods & method == 0 {
            return None;
        }
        let cidrs = generation.cidrs[index];
        if !cidrs.is_null() && unsafe { ngx_cidr_match(sockaddr, cidrs) } != OK {
            return None;
        }
        Some(if host_router { rank } else { 0 })
    });
    let Some((index, _)) = found else { return NGX_HTTP_NOT_FOUND as ngx_int_t };
    state.route = Some(index);
    match core.routes[index].resolve_status {
        0 => OK,
        status => status as ngx_int_t,
    }
}

/// `ac_upstream_prepare_native`: proxied scheme/host/URI and the first peer.
unsafe fn upstream(r: *mut ngx_http_request_t, generation: &GenRt, state: &mut State) -> ngx_int_t {
    let Some(index) = state.route else { return SERVER_ERROR };
    let Some(upstream) = generation.core.routes[index].upstream.as_ref() else {
        return NGX_HTTP_SERVICE_UNAVAILABLE as ngx_int_t;
    };
    if upstream.nodes.is_empty() {
        return NGX_HTTP_SERVICE_UNAVAILABLE as ngx_int_t;
    }
    unsafe {
        state.vars.set_bytes(vars::UPSTREAM_SCHEME, b"http");
        match upstream.rewrite_host.as_deref() {
            Some(host) => state.vars.set_bytes(vars::UPSTREAM_HOST, host),
            None => {
                let host = (*r).headers_in.host;
                state.vars.set(vars::UPSTREAM_HOST, if host.is_null() { (*r).headers_in.server } else { (*host).value });
            }
        }
        if (*r).valid_unparsed_uri() != 0 {
            state.vars.set(vars::UPSTREAM_URI, (*r).unparsed_uri);
        } else {
            let args = http::bytes(&(*r).args);
            let suffix = if args.is_empty() { None } else { Some(args) };
            let Some(uri) = http::escaped(r, http::bytes(&(*r).uri), NGX_ESCAPE_URI, suffix) else {
                return SERVER_ERROR;
            };
            state.vars.set(vars::UPSTREAM_URI, uri);
        }
    }
    let mut peer = Peer::new(upstream.nodes.len(), unsafe { ngx_current_msec } as u64);
    let Some(node) = upstream.pick(&mut peer, unsafe { ngx_current_msec } as u64) else {
        return NGX_HTTP_BAD_GATEWAY as ngx_int_t;
    };
    state.vars.set_bytes(vars::UPSTREAM_UNRESOLVED_HOST, &upstream.nodes[node].host);
    state.peer = Some(peer);
    OK
}

pub(crate) unsafe extern "C" fn log_handler(r: *mut ngx_http_request_t) -> ngx_int_t {
    let log = unsafe { (*(*r).connection).log };
    guard::contain(log, OK, "log handler", || unsafe {
        let Some(ctx) = RequestCtx::get_mut(r) else { return OK };
        let RequestCtx { generation, state, .. } = ctx;
        if state.logged {
            return OK;
        }
        state.logged = true;
        let Some(upstream) = state.route.and_then(|index| generation.core.routes[index].upstream.as_ref()) else {
            return OK;
        };
        let u = (*r).upstream;
        let last = if u.is_null() { ptr::null_mut() } else { (*u).state };
        if let Some(peer) = state.peer.as_mut() {
            let failed = !last.is_null() && ((*last).status == 502 || (*last).status == 504);
            upstream.release(peer, failed, ngx_current_msec as u64);
        }
        if !last.is_null() && !(*last).peer.is_null() {
            upstream.record_response_time(http::bytes(&*(*last).peer), (*last).response_time as f64);
        }
        OK
    })
}
