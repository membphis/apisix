// SPDX-License-Identifier: Apache-2.0
//! Native key-auth, ported from `module/key_auth.c` (`ac_key_execute`,
//! `ac_key_auth_run`): header lookup, URI-argument fallback, anonymous
//! consumer, hidden credentials and the three consumer request headers.

use core::ptr;

use apisix_rs_core::config::KeyAuth;
use ngx::ffi::*;

use crate::http;
use crate::pipeline::State;
use crate::runtime::GenRt;

pub(crate) const MISSING: &[u8] = b"{\"message\":\"Missing API key in request\"}\n";
pub(crate) const INVALID: &[u8] = b"{\"message\":\"Invalid API key in request\"}\n";
pub(crate) const UNAUTHORIZED: &[u8] = b"{\"message\":\"Invalid user authorization\"}\n";

const SERVER_ERROR: ngx_int_t = NGX_HTTP_INTERNAL_SERVER_ERROR as ngx_int_t;

struct Arg {
    key: Vec<u8>,
    value: Vec<u8>,
    boolean: bool,
}

/// `ac_key_decode`: `+` to space, then `ngx_unescape_uri(..., 0)`.
unsafe fn decode(raw: &[u8]) -> Vec<u8> {
    let mut out: Vec<u8> = raw.iter().map(|&b| if b == b'+' { b' ' } else { b }).collect();
    unsafe {
        let start = out.as_mut_ptr();
        let (mut dst, mut src) = (start, start);
        ngx_unescape_uri(&mut dst, &mut src, out.len(), 0);
        let len = dst.offset_from(start) as usize;
        out.truncate(len);
    }
    out
}

/// `ac_key_args`: `ngx.req.get_uri_args(0)` (unlimited), keeping order.
unsafe fn args(r: *mut ngx_http_request_t) -> Vec<Arg> {
    let raw = unsafe { http::bytes(&(*r).args) };
    let mut out = Vec::new();
    for item in raw.split(|&b| b == b'&') {
        let (key, value) = match item.iter().position(|&b| b == b'=') {
            Some(equal) => (&item[..equal], Some(&item[equal + 1..])),
            None => (item, None),
        };
        if key.is_empty() {
            continue;
        }
        out.push(Arg {
            key: unsafe { decode(key) },
            value: value.map(|v| unsafe { decode(v) }).unwrap_or_default(),
            boolean: value.is_none(),
        });
    }
    out
}

/// `ac_key_hide_query`: re-encode every argument except the credential and
/// install the result as `r->args`.
unsafe fn hide_query(r: *mut ngx_http_request_t, args: &[Arg], name: &[u8]) -> bool {
    let component = NGX_ESCAPE_URI_COMPONENT as ngx_uint_t;
    unsafe {
        let escape_len = |s: &[u8]| s.len() + 3 * ngx_escape_uri(ptr::null_mut(), s.as_ptr() as *mut u8, s.len(), component);
        let mut length = 0;
        for arg in args.iter().filter(|a| a.key != name) {
            length += escape_len(&arg.key) + 2;
            if !arg.boolean {
                length += escape_len(&arg.value);
            }
        }
        let Some(out) = http::pool_bytes(r, length + 1) else { return false };
        let start = out.as_mut_ptr();
        let mut end = start;
        for arg in args.iter().filter(|a| a.key != name) {
            if end != start {
                *end = b'&';
                end = end.add(1);
            }
            end = ngx_escape_uri(end, arg.key.as_ptr() as *mut u8, arg.key.len(), component) as *mut u8;
            if !arg.boolean {
                *end = b'=';
                end = end.add(1);
                end = ngx_escape_uri(end, arg.value.as_ptr() as *mut u8, arg.value.len(), component) as *mut u8;
            }
        }
        (*r).args = ngx_str_t { len: end.offset_from(start) as usize, data: start };
        (*r).set_valid_unparsed_uri(0);
    }
    true
}

/// Runs key-auth for the matched route. On rejection returns 401 and sets
/// `body`; on success records the consumer in `state`.
pub(crate) unsafe fn run(
    r: *mut ngx_http_request_t,
    generation: &GenRt,
    conf: &KeyAuth,
    state: &mut State,
    body: &mut &'static [u8],
) -> ngx_int_t {
    let log = unsafe { (*(*r).connection).log };
    let header = unsafe { http::request_header(r, &conf.header) };
    let mut parsed: Option<Vec<Arg>> = None;
    let mut key: &[u8] = header.unwrap_or_default();
    let mut found = 0;
    let mut invalid_type = false;
    if header.is_none() {
        let list = parsed.insert(unsafe { args(r) });
        for arg in list.iter() {
            if arg.key == conf.query {
                key = &arg.value;
                found += 1;
                invalid_type = arg.boolean;
            }
        }
        if found > 1 {
            invalid_type = true;
        }
    }
    let missing = header.is_none() && found == 0;
    let mut consumer = if !missing && !invalid_type { generation.core.keys.find(key) } else { None };

    if consumer.is_none() {
        *body = if missing { MISSING } else { INVALID };
        if !missing {
            ngx::ngx_log_error!(NGX_LOG_WARN, log, "failed to find consumer: invalid api key");
        }
        if let Some(anonymous) = &conf.anonymous {
            if conf.hide {
                if header.is_some() && unsafe { !http::set_request_header(r, &conf.header, None) } {
                    return SERVER_ERROR;
                }
                let list = parsed.get_or_insert_with(|| unsafe { args(r) });
                // set_uri_args is only called when the credential field exists.
                if list.iter().any(|arg| arg.key == conf.query) && unsafe { !hide_query(r, list, &conf.query) } {
                    return SERVER_ERROR;
                }
            }
            consumer = anonymous.consumer;
            if consumer.is_none() {
                *body = UNAUTHORIZED;
                ngx::ngx_log_error!(
                    NGX_LOG_ERR,
                    log,
                    "key-auth failed to authenticate the request, code: 401. error: failed to get anonymous consumer {}",
                    String::from_utf8_lossy(&anonymous.name)
                );
            }
        }
        if consumer.is_none() {
            if unsafe { !http::set_response_header(r, b"WWW-Authenticate", &conf.challenge) } {
                ngx::ngx_log_error!(NGX_LOG_ERR, log, "native key-auth header failed: invalid native response header");
                return SERVER_ERROR;
            }
            return NGX_HTTP_UNAUTHORIZED as ngx_int_t;
        }
    } else if conf.hide {
        let hidden = match (header, parsed.as_ref()) {
            (Some(_), _) => unsafe { http::set_request_header(r, &conf.header, None) },
            (None, Some(list)) => unsafe { hide_query(r, list, &conf.query) },
            (None, None) => true,
        };
        if !hidden {
            return SERVER_ERROR;
        }
    }

    let Some(index) = consumer else { return SERVER_ERROR };
    let Some(record) = generation.core.consumers.get(index) else { return SERVER_ERROR };
    let headers: [(&[u8], &Option<Vec<u8>>); 3] = [
        (b"X-Consumer-Username", &record.username),
        (b"X-Credential-Identifier", &record.credential_id),
        (b"X-Consumer-Custom-ID", &record.custom_id),
    ];
    for (name, value) in headers {
        if unsafe { !http::set_request_header(r, name, value.as_deref()) } {
            ngx::ngx_log_error!(NGX_LOG_ERR, log, "native key-auth header failed");
            return SERVER_ERROR;
        }
    }
    state.consumer = Some(index);
    ngx::ngx_log_error!(NGX_LOG_INFO, log, "hit key-auth rewrite");
    NGX_OK as ngx_int_t
}
