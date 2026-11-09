// SPDX-License-Identifier: Apache-2.0
//! Request/response primitives shared by the pipeline. Each helper mirrors the
//! C primitive the v2 pipeline uses, including the same exported runtime
//! functions, so both implementations perform the same NGINX-level work.

use core::ffi::{c_char, c_int, c_void};
use core::{ptr, slice};

use ngx::ffi::*;

extern "C" {
    /// Exported by ngx_lua (used by resty.core and by the C module's
    /// `ac_native_header_write`). `value == NULL` removes the header.
    fn ngx_http_lua_ffi_req_set_header(
        r: *mut ngx_http_request_t,
        key: *const u8,
        key_len: usize,
        value: *const u8,
        value_len: usize,
        mvals: *mut c_void,
        mvals_len: usize,
        override_: c_int,
        errmsg: *mut *mut c_char,
    ) -> c_int;
}

/// Bytes of an `ngx_str_t`; empty for a null string.
#[inline]
pub(crate) unsafe fn bytes<'a>(value: &ngx_str_t) -> &'a [u8] {
    if value.data.is_null() || value.len == 0 {
        &[]
    } else {
        unsafe { slice::from_raw_parts(value.data, value.len) }
    }
}

#[inline]
pub(crate) fn str_of(value: &[u8]) -> ngx_str_t {
    ngx_str_t { len: value.len(), data: value.as_ptr() as *mut u8 }
}

/// `ngx_hash_key`, evaluated at compile time for fixed variable names.
pub(crate) const fn hash(name: &[u8]) -> ngx_uint_t {
    let mut key: ngx_uint_t = 0;
    let mut index = 0;
    while index < name.len() {
        key = key.wrapping_mul(31).wrapping_add(name[index] as ngx_uint_t);
        index += 1;
    }
    key
}

/// `ngx_http_get_variable` by name; `None` when not found.
#[inline]
pub(crate) unsafe fn variable<'a>(r: *mut ngx_http_request_t, name: &'static [u8], key: ngx_uint_t) -> Option<&'a [u8]> {
    let mut name = str_of(name);
    let value = unsafe { ngx_http_get_variable(r, &mut name, key) };
    if value.is_null() || unsafe { (*value).not_found() } != 0 {
        return None;
    }
    let len = unsafe { (*value).len() } as usize;
    Some(if len == 0 || unsafe { (*value).data }.is_null() {
        &[]
    } else {
        unsafe { slice::from_raw_parts((*value).data, len) }
    })
}

/// Request pool allocation (`ngx_pnalloc`).
#[inline]
pub(crate) unsafe fn pool_bytes<'a>(r: *mut ngx_http_request_t, len: usize) -> Option<&'a mut [u8]> {
    let data = unsafe { ngx_pnalloc((*r).pool, len.max(1)) } as *mut u8;
    if data.is_null() {
        None
    } else {
        Some(unsafe { slice::from_raw_parts_mut(data, len) })
    }
}

/// Iterates `ngx_table_elt_t` entries of an NGINX list, including deleted ones.
pub(crate) unsafe fn for_each_header(list: &ngx_list_t, mut visit: impl FnMut(&mut ngx_table_elt_t) -> bool) {
    let mut part: *const ngx_list_part_t = &list.part;
    while !part.is_null() {
        let elements = unsafe { (*part).elts } as *mut ngx_table_elt_t;
        for index in 0..unsafe { (*part).nelts } {
            if !visit(unsafe { &mut *elements.add(index) }) {
                return;
            }
        }
        part = unsafe { (*part).next };
    }
}

/// `ac_key_header`: `ngx.req.get_headers()` semantics as used by the APISIX
/// SDK: only the first 100 entries are visible, the first duplicate wins, and
/// a name containing `_` is retried with `-`.
pub(crate) unsafe fn request_header<'a>(r: *mut ngx_http_request_t, name: &[u8]) -> Option<&'a [u8]> {
    let has_underscore = name.contains(&b'_');
    for pass in 0..2 {
        let mut found: Option<&[u8]> = None;
        let mut count = 0;
        unsafe {
            for_each_header(&(*r).headers_in.headers, |header| {
                count += 1;
                if count > 100 {
                    return false;
                }
                if header.hash == 0 || header.key.len != name.len() {
                    return true;
                }
                let key = bytes(&header.key);
                let same = name.iter().zip(key).all(|(&wanted, &actual)| {
                    let wanted = if pass == 1 && wanted == b'_' { b'-' } else { wanted.to_ascii_lowercase() };
                    wanted == actual.to_ascii_lowercase()
                });
                if same {
                    found = Some(if header.value.data.is_null() {
                        &[]
                    } else {
                        slice::from_raw_parts(header.value.data, header.value.len)
                    });
                    return false;
                }
                true
            });
        }
        if found.is_some() || !has_underscore {
            return found;
        }
    }
    None
}

/// ngx_lua's exported request header setter (`value == NULL` removes).
#[inline]
pub(crate) unsafe fn req_set_header(
    r: *mut ngx_http_request_t,
    name: &[u8],
    value: *const u8,
    value_len: usize,
    override_: c_int,
    error: *mut *mut c_char,
) -> c_int {
    unsafe {
        ngx_http_lua_ffi_req_set_header(r, name.as_ptr(), name.len(), value, value_len, ptr::null_mut(), 0, override_, error)
    }
}

/// Sets (`Some`) or removes (`None`) a request header through ngx_lua's
/// exported setter, exactly as `ac_native_header_write(..., response = 0)`.
#[inline]
pub(crate) unsafe fn set_request_header(r: *mut ngx_http_request_t, name: &[u8], value: Option<&[u8]>) -> bool {
    let mut error: *mut c_char = ptr::null_mut();
    let (data, len) = match value {
        Some(value) => (value.as_ptr(), value.len()),
        None => (ptr::null(), 0),
    };
    let status = unsafe {
        ngx_http_lua_ffi_req_set_header(r, name.as_ptr(), name.len(), data, len, ptr::null_mut(), 0, 1, &mut error)
    };
    status == NGX_OK as c_int || status == NGX_DECLINED as c_int
}

/// Pipeline-mode response header write: hide previous same-name entries and
/// append the new one (`ac_native_header_write(..., response = 1)`).
pub(crate) unsafe fn set_response_header(r: *mut ngx_http_request_t, name: &'static [u8], value: &[u8]) -> bool {
    if name.contains(&b':') || value.iter().any(|&b| b == b'\r' || b == b'\n') {
        return false;
    }
    unsafe {
        for_each_header(&(*r).headers_out.headers, |header| {
            if header.key.len == name.len() && bytes(&header.key).eq_ignore_ascii_case(name) {
                header.hash = 0;
            }
            true
        });
        let header = ngx_list_push(&mut (*r).headers_out.headers) as *mut ngx_table_elt_t;
        if header.is_null() {
            return false;
        }
        ptr::write_bytes(header, 0, 1);
        (*header).hash = 1;
        (*header).key = str_of(name);
        (*header).value = str_of(value);
        // As in C: the stock header filter emits its own Server line unless set.
        if name.eq_ignore_ascii_case(b"server") {
            (*r).headers_out.server = header;
        }
    }
    true
}

/// Rejection response of `ac_pipeline_access`: JSON body, discarded request
/// body, explicit finalization for HEAD as for GET.
pub(crate) unsafe fn send_json(r: *mut ngx_http_request_t, status: ngx_int_t, body: &'static [u8]) -> ngx_int_t {
    unsafe {
        (*r).headers_out.status = status as ngx_uint_t;
        (*r).headers_out.content_length_n = body.len() as off_t;
        (*r).headers_out.content_type = str_of(b"application/json");
        if ngx_http_discard_request_body(r) != NGX_OK as ngx_int_t {
            return NGX_HTTP_INTERNAL_SERVER_ERROR as ngx_int_t;
        }
        let rc = ngx_http_send_header(r);
        if rc == NGX_ERROR as ngx_int_t || rc > NGX_OK as ngx_int_t {
            return rc;
        }
        if (*r).header_only() != 0 {
            ngx_http_finalize_request(r, NGX_OK as ngx_int_t);
            return NGX_DONE as ngx_int_t;
        }
        let buffer = ngx_pcalloc((*r).pool, core::mem::size_of::<ngx_buf_t>()) as *mut ngx_buf_t;
        if buffer.is_null() {
            return NGX_HTTP_INTERNAL_SERVER_ERROR as ngx_int_t;
        }
        (*buffer).pos = body.as_ptr() as *mut u8;
        (*buffer).last = (*buffer).pos.add(body.len());
        (*buffer).set_memory(1);
        (*buffer).set_last_buf(u32::from(r == (*r).main));
        (*buffer).set_last_in_chain(1);
        let mut chain = ngx_chain_t { buf: buffer, next: ptr::null_mut() };
        let rc = ngx_http_output_filter(r, &mut chain);
        ngx_http_finalize_request(r, rc);
        NGX_DONE as ngx_int_t
    }
}

/// `ngx_escape_uri` into the request pool; `prefix` and `suffix` are copied
/// around the escaped `source` (used for `uri?args` and query rebuilding).
pub(crate) unsafe fn escaped(
    r: *mut ngx_http_request_t,
    source: &[u8],
    kind: u32,
    suffix: Option<&[u8]>,
) -> Option<ngx_str_t> {
    unsafe {
        let extra = ngx_escape_uri(ptr::null_mut(), source.as_ptr() as *mut u8, source.len(), kind as ngx_uint_t);
        let tail = suffix.map_or(0, |s| s.len() + 1);
        let out = pool_bytes(r, source.len() + 2 * extra + tail)?;
        let start = out.as_mut_ptr();
        let mut end = ngx_escape_uri(start, source.as_ptr() as *mut u8, source.len(), kind as ngx_uint_t) as *mut u8;
        if let Some(suffix) = suffix {
            *end = b'?';
            end = end.add(1);
            ptr::copy_nonoverlapping(suffix.as_ptr(), end, suffix.len());
            end = end.add(suffix.len());
        }
        Some(ngx_str_t { len: end.offset_from(start) as usize, data: start })
    }
}
