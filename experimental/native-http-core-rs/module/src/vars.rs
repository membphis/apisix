// SPDX-License-Identifier: Apache-2.0
//! NGINX variables backed by the request context, as `ac_var_get`/`ac_var_set`
//! do for the C pipeline: `proxy_pass $upstream_scheme://apisix_backend$upstream_uri`
//! and `proxy_set_header Host $upstream_host` read the values the access
//! handler computed. They are registered in preconfiguration so the APISIX
//! template's `set $upstream_* ...` lines bind to these changeable variables;
//! those assignments run before the context exists and are ignored.

use core::ptr;

use ngx::ffi::*;

use crate::pipeline::RequestCtx;

pub(crate) const UPSTREAM_SCHEME: usize = 0;
pub(crate) const UPSTREAM_HOST: usize = 1;
pub(crate) const UPSTREAM_URI: usize = 2;
pub(crate) const UPSTREAM_UNRESOLVED_HOST: usize = 3;
pub(crate) const REQUEST_LINE: usize = 4;
pub(crate) const ORIGINAL_X_FORWARDED_FOR: usize = 5;
/// `original_x_forwarded_proto/host/port` and `original_forwarded` follow in order.
pub(crate) const ORIGINAL_FORWARDING: usize = 6;
pub(crate) const COUNT: usize = 10;

const NAMES: [&str; COUNT] = [
    "upstream_scheme",
    "upstream_host",
    "upstream_uri",
    "upstream_unresolved_host",
    "request_line",
    "original_x_forwarded_for",
    "original_x_forwarded_proto",
    "original_x_forwarded_host",
    "original_x_forwarded_port",
    "original_forwarded",
];

const DEFAULTS: [&[u8]; COUNT] = [b"http", b"", b"", b"", b"", b"", b"", b"", b"", b""];

/// Per-request values; a null `data` means "not assigned".
pub(crate) struct Values {
    values: [ngx_str_t; COUNT],
    assigned: u16,
}

impl Values {
    pub(crate) fn new() -> Values {
        Values { values: [ngx_str_t { len: 0, data: ptr::null_mut() }; COUNT], assigned: 0 }
    }

    #[inline]
    pub(crate) fn set(&mut self, slot: usize, value: ngx_str_t) {
        self.values[slot] = value;
        self.assigned |= 1 << slot;
    }

    #[inline]
    pub(crate) fn set_bytes(&mut self, slot: usize, value: &[u8]) {
        self.set(slot, ngx_str_t { len: value.len(), data: value.as_ptr() as *mut u8 });
    }

    /// The assigned value of `slot`, if any (request-pool or generation memory).
    #[inline]
    pub(crate) fn get<'a>(&self, slot: usize) -> Option<&'a [u8]> {
        let value = self.values[slot];
        if value.data.is_null() {
            None
        } else {
            Some(unsafe { core::slice::from_raw_parts(value.data, value.len) })
        }
    }
}

pub(crate) unsafe fn register(cf: *mut ngx_conf_t) -> ngx_int_t {
    for (slot, name) in NAMES.iter().enumerate() {
        let mut key = ngx_str_t { len: name.len(), data: name.as_ptr() as *mut u8 };
        let variable =
            unsafe { ngx_http_add_variable(cf, &mut key, (NGX_HTTP_VAR_CHANGEABLE | NGX_HTTP_VAR_NOCACHEABLE) as ngx_uint_t) };
        if variable.is_null() {
            return NGX_ERROR as ngx_int_t;
        }
        unsafe {
            (*variable).get_handler = Some(get_handler);
            (*variable).set_handler = Some(set_handler);
            (*variable).data = slot;
        }
    }
    NGX_OK as ngx_int_t
}

unsafe extern "C" fn get_handler(r: *mut ngx_http_request_t, value: *mut ngx_variable_value_t, slot: usize) -> ngx_int_t {
    let log = unsafe { (*(*r).connection).log };
    crate::guard::contain(log, NGX_ERROR as ngx_int_t, "variable get", || unsafe {
        let ctx = RequestCtx::get(r);
        let mut result = ctx.map(|ctx| ctx.state.vars.values[slot]).filter(|v| !v.data.is_null());
        if result.is_none() {
            if ctx.is_some_and(|ctx| ctx.state.vars.assigned & (1 << slot) != 0) {
                (*value).set_not_found(1);
                return NGX_OK as ngx_int_t;
            }
            let host = (*r).headers_in.host;
            result = Some(if slot == UPSTREAM_HOST && !host.is_null() {
                (*host).value
            } else {
                ngx_str_t { len: DEFAULTS[slot].len(), data: DEFAULTS[slot].as_ptr() as *mut u8 }
            });
        }
        let result = result.unwrap_or(ngx_str_t { len: 0, data: ptr::null_mut() });
        (*value).set_valid(1);
        (*value).set_no_cacheable(1);
        (*value).set_not_found(0);
        (*value).set_len(result.len as u32);
        (*value).data = result.data;
        NGX_OK as ngx_int_t
    })
}

unsafe extern "C" fn set_handler(r: *mut ngx_http_request_t, value: *mut ngx_variable_value_t, slot: usize) {
    let log = unsafe { (*(*r).connection).log };
    crate::guard::contain(log, (), "variable set", || unsafe {
        if let Some(ctx) = RequestCtx::get_mut(r) {
            let len = (*value).len() as usize;
            ctx.state.vars.set(slot, ngx_str_t { len, data: (*value).data });
        }
    });
}
