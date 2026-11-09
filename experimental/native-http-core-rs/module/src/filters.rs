// SPDX-License-Identifier: Apache-2.0
//! Native response header filter: `ac_native_header` / `ac_header_fields` in
//! `module/native_phases.c`, which both C variants run (`apisix_core_native_filters`
//! is required by the C pipeline and on for C native-lua).
//!
//! For requests that entered the core in a proxied location it sets
//! `Server: APISIX/3.19.0` (`APISIX` without server tokens) and, for a 5xx
//! upstream status or when configured, `X-APISIX-Upstream-Status`.
//! The pipeline writes the header list directly, as the C pipeline does;
//! step-1 requests use ngx_lua's response header setter, as C native-lua does.
//! C's native body filter does no work in either mode, so none is installed.

use core::ptr;

use ngx::ffi::*;
use ngx::http::HttpModuleConfExt;

use crate::pipeline::RequestCtx;
use crate::{guard, http, module, sdk, LocConf};

static mut NEXT_HEADER: ngx_http_output_header_filter_pt = None;

/// Installs the filter (postconfiguration, once per configuration cycle).
pub(crate) unsafe fn register() {
    unsafe {
        NEXT_HEADER = *ptr::addr_of!(ngx_http_top_header_filter);
        ngx_http_top_header_filter = Some(header_filter);
    }
}

unsafe fn write(r: *mut ngx_http_request_t, pipeline: bool, name: &'static [u8], value: &[u8]) -> bool {
    unsafe {
        if pipeline {
            http::set_response_header(r, name, value)
        } else {
            sdk::native_header_write(r, name, Some(value), true, false).is_ok()
        }
    }
}

/// `ac_header_fields`.
unsafe fn fields(r: *mut ngx_http_request_t) -> bool {
    unsafe {
        let Some(ctx) = RequestCtx::get(r) else { return true };
        let enabled = (*r)
            .http_location_conf_unchecked::<LocConf>(module())
            .is_some_and(|conf| (*conf.as_ptr()).proxy == 1);
        if !enabled {
            return true;
        }
        let settings = ctx.generation.core.settings;
        let pipeline = ctx.lua.is_none();
        let server: &'static [u8] = if settings.server_tokens { b"APISIX/3.19.0" } else { b"APISIX" };
        if !write(r, pipeline, b"Server", server) {
            return false;
        }
        if let Some(status) = http::variable(r, b"upstream_status", http::hash(b"upstream_status")) {
            let last = if status.len() >= 3 {
                std::str::from_utf8(&status[status.len() - 3..]).ok().and_then(|text| text.parse::<u32>().ok())
            } else {
                None
            };
            if settings.show_upstream_status || last.is_some_and(|code| (500..=599).contains(&code)) {
                return write(r, pipeline, b"X-APISIX-Upstream-Status", status);
            }
        }
        true
    }
}

unsafe extern "C" fn header_filter(r: *mut ngx_http_request_t) -> ngx_int_t {
    let log = unsafe { (*(*r).connection).log };
    let ok = guard::contain(log, false, "header filter", || unsafe { fields(r) });
    if !ok {
        ngx::ngx_log_error!(NGX_LOG_ERR, log, "native header processing failed");
        return NGX_ERROR as ngx_int_t;
    }
    match unsafe { *ptr::addr_of!(NEXT_HEADER) } {
        Some(next) => unsafe { next(r) },
        None => NGX_OK as ngx_int_t,
    }
}
