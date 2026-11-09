// SPDX-License-Identifier: Apache-2.0
//! Step 1 (Rust core + unchanged Lua plugins): the LuaJIT C API and the
//! ngx_lua exports the C prototype's Lua ABI adapter uses.
//!
//! These are raw declarations of the pinned runtime's C ABI, written by hand
//! like the C prototype's own `extern` declarations (equivalent to the raw
//! `mlua-sys` bindings, without adding a dependency). Every function here is
//! declared `C-unwind`: LuaJIT raises errors by unwinding (x86-64 external
//! unwinding), so a Lua error may cross Rust frames, running their destructors.
//! Lua-facing Rust functions are therefore `extern "C-unwind"` too and never
//! use `catch_unwind` (a Lua error unwinding through it aborts). A Rust panic
//! that escapes such a function aborts the worker: LuaJIT would catch it, but
//! Rust aborts when foreign code discards a panic ("Rust panics must be
//! rethrown"). The blast radius equals a crash in the C adapter.

#![allow(non_camel_case_types, non_snake_case, dead_code)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use ngx::ffi::{ngx_conf_t, ngx_http_request_t, ngx_int_t, ngx_str_t};

#[repr(C)]
pub(crate) struct lua_State {
    _private: [u8; 0],
}

pub(crate) type lua_CFunction = unsafe extern "C-unwind" fn(*mut lua_State) -> c_int;
pub(crate) type lua_Number = f64;
pub(crate) type lua_Integer = isize;

pub(crate) const LUA_REGISTRYINDEX: c_int = -10000;
pub(crate) const LUA_GLOBALSINDEX: c_int = -10002;
pub(crate) const LUA_MULTRET: c_int = -1;
pub(crate) const LUA_NOREF: c_int = -2;
pub(crate) const LUA_REFNIL: c_int = -1;

pub(crate) const LUA_TNONE: c_int = -1;
pub(crate) const LUA_TNIL: c_int = 0;
pub(crate) const LUA_TBOOLEAN: c_int = 1;
pub(crate) const LUA_TLIGHTUSERDATA: c_int = 2;
pub(crate) const LUA_TNUMBER: c_int = 3;
pub(crate) const LUA_TSTRING: c_int = 4;
pub(crate) const LUA_TTABLE: c_int = 5;
pub(crate) const LUA_TFUNCTION: c_int = 6;

extern "C-unwind" {
    pub(crate) fn lua_gettop(L: *mut lua_State) -> c_int;
    pub(crate) fn lua_settop(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_pushvalue(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_remove(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_insert(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_replace(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_type(L: *mut lua_State, index: c_int) -> c_int;
    pub(crate) fn lua_isnumber(L: *mut lua_State, index: c_int) -> c_int;
    pub(crate) fn lua_isstring(L: *mut lua_State, index: c_int) -> c_int;
    pub(crate) fn lua_equal(L: *mut lua_State, a: c_int, b: c_int) -> c_int;
    pub(crate) fn lua_rawequal(L: *mut lua_State, a: c_int, b: c_int) -> c_int;
    pub(crate) fn lua_tonumber(L: *mut lua_State, index: c_int) -> lua_Number;
    pub(crate) fn lua_tointeger(L: *mut lua_State, index: c_int) -> lua_Integer;
    pub(crate) fn lua_toboolean(L: *mut lua_State, index: c_int) -> c_int;
    pub(crate) fn lua_tolstring(L: *mut lua_State, index: c_int, len: *mut usize) -> *const c_char;
    pub(crate) fn lua_objlen(L: *mut lua_State, index: c_int) -> usize;
    pub(crate) fn lua_touserdata(L: *mut lua_State, index: c_int) -> *mut c_void;
    pub(crate) fn lua_topointer(L: *mut lua_State, index: c_int) -> *const c_void;
    pub(crate) fn lua_pushnil(L: *mut lua_State);
    pub(crate) fn lua_pushnumber(L: *mut lua_State, n: lua_Number);
    pub(crate) fn lua_pushinteger(L: *mut lua_State, n: lua_Integer);
    pub(crate) fn lua_pushlstring(L: *mut lua_State, s: *const c_char, len: usize);
    pub(crate) fn lua_pushcclosure(L: *mut lua_State, f: lua_CFunction, n: c_int);
    pub(crate) fn lua_pushboolean(L: *mut lua_State, b: c_int);
    pub(crate) fn lua_pushlightuserdata(L: *mut lua_State, p: *mut c_void);
    pub(crate) fn lua_pushthread(L: *mut lua_State) -> c_int;
    pub(crate) fn lua_gettable(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_getfield(L: *mut lua_State, index: c_int, k: *const c_char);
    pub(crate) fn lua_rawget(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_rawgeti(L: *mut lua_State, index: c_int, n: c_int);
    pub(crate) fn lua_createtable(L: *mut lua_State, narr: c_int, nrec: c_int);
    pub(crate) fn lua_getmetatable(L: *mut lua_State, index: c_int) -> c_int;
    pub(crate) fn lua_settable(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_setfield(L: *mut lua_State, index: c_int, k: *const c_char);
    pub(crate) fn lua_rawset(L: *mut lua_State, index: c_int);
    pub(crate) fn lua_rawseti(L: *mut lua_State, index: c_int, n: c_int);
    pub(crate) fn lua_setmetatable(L: *mut lua_State, index: c_int) -> c_int;
    pub(crate) fn lua_call(L: *mut lua_State, nargs: c_int, nresults: c_int);
    pub(crate) fn lua_pcall(L: *mut lua_State, nargs: c_int, nresults: c_int, errfunc: c_int) -> c_int;
    pub(crate) fn lua_error(L: *mut lua_State) -> !;
    pub(crate) fn lua_next(L: *mut lua_State, index: c_int) -> c_int;
    pub(crate) fn lua_concat(L: *mut lua_State, n: c_int);

    pub(crate) fn luaL_ref(L: *mut lua_State, t: c_int) -> c_int;
    pub(crate) fn luaL_unref(L: *mut lua_State, t: c_int, r: c_int);
    pub(crate) fn luaL_where(L: *mut lua_State, level: c_int);
    pub(crate) fn luaL_checknumber(L: *mut lua_State, arg: c_int) -> lua_Number;
    pub(crate) fn luaL_checklstring(L: *mut lua_State, arg: c_int, len: *mut usize) -> *const c_char;
    pub(crate) fn luaL_checktype(L: *mut lua_State, arg: c_int, t: c_int);
}

// ngx_lua's public C API (ngx_http_lua_api.h) and the FFI primitives exported
// for lua-resty-core, as declared by the C prototype.
extern "C-unwind" {
    pub(crate) fn ngx_http_lua_add_package_preload(cf: *mut ngx_conf_t, package: *const c_char, f: lua_CFunction)
        -> ngx_int_t;
    pub(crate) fn ngx_http_lua_get_request(L: *mut lua_State) -> *mut ngx_http_request_t;
}

extern "C" {
    pub(crate) fn ngx_http_lua_ffi_balancer_set_current_peer(
        r: *mut ngx_http_request_t,
        addr: *const u8,
        addr_len: usize,
        port: c_int,
        pool_crc32: u32,
        pool_size: u32,
        err: *mut *mut c_char,
    ) -> c_int;
    pub(crate) fn ngx_http_lua_ffi_balancer_enable_keepalive(
        r: *mut ngx_http_request_t,
        timeout: core::ffi::c_ulong,
        max_requests: u32,
        err: *mut *mut c_char,
    ) -> c_int;
    pub(crate) fn ngx_http_lua_ffi_balancer_set_more_tries(r: *mut ngx_http_request_t, count: c_int, err: *mut *mut c_char)
        -> c_int;
    pub(crate) fn ngx_http_lua_ffi_balancer_get_last_failure(
        r: *mut ngx_http_request_t,
        status: *mut c_int,
        err: *mut *mut c_char,
    ) -> c_int;
    pub(crate) fn ngx_http_lua_ffi_balancer_set_timeouts(
        r: *mut ngx_http_request_t,
        connect: core::ffi::c_long,
        send: core::ffi::c_long,
        read: core::ffi::c_long,
        err: *mut *mut c_char,
    ) -> c_int;
    pub(crate) fn ngx_http_lua_ffi_balancer_recreate_request(r: *mut ngx_http_request_t, err: *mut *mut c_char) -> c_int;
    pub(crate) fn ngx_http_lua_ffi_var_set(
        r: *mut ngx_http_request_t,
        name: *mut u8,
        name_len: usize,
        lowcase: *mut u8,
        value: *mut u8,
        value_len: usize,
        errbuf: *mut u8,
        errlen: *mut usize,
    ) -> c_int;
    pub(crate) fn ngx_http_lua_ffi_set_resp_header(
        r: *mut ngx_http_request_t,
        key: *const c_char,
        key_len: usize,
        is_nil: c_int,
        value: *const c_char,
        value_len: usize,
        mvals: *mut c_void,
        mvals_len: usize,
        override_: c_int,
        err: *mut *mut c_char,
    ) -> c_int;
    pub(crate) fn ngx_http_lua_ffi_get_resp_status(r: *mut ngx_http_request_t) -> c_int;
    // api7 ngxvar exports (also used by resty.ngxvar).
    pub(crate) fn ngx_http_lua_var_ffi_uri(r: *mut ngx_http_request_t, value: *mut ngx_str_t) -> c_int;
    pub(crate) fn ngx_http_lua_var_ffi_host(r: *mut ngx_http_request_t, value: *mut ngx_str_t) -> c_int;
    pub(crate) fn ngx_http_lua_var_ffi_remote_addr(r: *mut ngx_http_request_t, value: *mut ngx_str_t) -> c_int;
    pub(crate) fn ngx_http_lua_var_ffi_scheme(r: *mut ngx_http_request_t, value: *mut ngx_str_t) -> c_int;
    // apisix-nginx-module exports used by resty.apisix.request.
    pub(crate) fn ngx_http_apisix_is_request_header_set(r: *mut ngx_http_request_t) -> ngx_int_t;
    pub(crate) fn ngx_http_apisix_clear_request_header(r: *mut ngx_http_request_t);
}

/// NUL-terminated literal for the `*const c_char` parameters.
macro_rules! cstr {
    ($text:literal) => {
        concat!($text, "\0").as_ptr() as *const core::ffi::c_char
    };
}
pub(crate) use cstr;

#[inline]
pub(crate) const fn upvalue(index: c_int) -> c_int {
    LUA_GLOBALSINDEX - index
}

#[inline]
pub(crate) unsafe fn pop(L: *mut lua_State, count: c_int) {
    unsafe { lua_settop(L, -count - 1) }
}

#[inline]
pub(crate) unsafe fn new_table(L: *mut lua_State) {
    unsafe { lua_createtable(L, 0, 0) }
}

#[inline]
pub(crate) unsafe fn push_fn(L: *mut lua_State, f: lua_CFunction) {
    unsafe { lua_pushcclosure(L, f, 0) }
}

#[inline]
pub(crate) unsafe fn push_bytes(L: *mut lua_State, value: &[u8]) {
    unsafe { lua_pushlstring(L, value.as_ptr() as *const c_char, value.len()) }
}

#[inline]
pub(crate) unsafe fn push_str(L: *mut lua_State, value: &ngx_str_t) {
    unsafe { lua_pushlstring(L, value.data as *const c_char, value.len) }
}

#[inline]
pub(crate) unsafe fn is_nil(L: *mut lua_State, index: c_int) -> bool {
    unsafe { lua_type(L, index) == LUA_TNIL }
}

#[inline]
pub(crate) unsafe fn is_none_or_nil(L: *mut lua_State, index: c_int) -> bool {
    unsafe { lua_type(L, index) <= LUA_TNIL }
}

#[inline]
pub(crate) unsafe fn is_table(L: *mut lua_State, index: c_int) -> bool {
    unsafe { lua_type(L, index) == LUA_TTABLE }
}

#[inline]
pub(crate) unsafe fn is_function(L: *mut lua_State, index: c_int) -> bool {
    unsafe { lua_type(L, index) == LUA_TFUNCTION }
}

#[inline]
pub(crate) unsafe fn truthy(L: *mut lua_State, index: c_int) -> bool {
    unsafe { lua_toboolean(L, index) != 0 }
}

#[inline]
pub(crate) unsafe fn get_global(L: *mut lua_State, name: *const c_char) {
    unsafe { lua_getfield(L, LUA_GLOBALSINDEX, name) }
}

/// `ac_abs`: absolute stack index (pseudo indices unchanged).
#[inline]
pub(crate) unsafe fn absolute(L: *mut lua_State, index: c_int) -> c_int {
    if index > 0 || index <= LUA_REGISTRYINDEX {
        index
    } else {
        (unsafe { lua_gettop(L) }) + index + 1
    }
}

/// `ac_push`: pushes a registry reference (nil for NOREF/REFNIL).
#[inline]
pub(crate) unsafe fn push_ref(L: *mut lua_State, reference: c_int) {
    if reference == LUA_NOREF || reference == LUA_REFNIL {
        unsafe { lua_pushnil(L) }
    } else {
        unsafe { lua_rawgeti(L, LUA_REGISTRYINDEX, reference) }
    }
}

/// `luaL_ref` of the value at `index` (the value stays on the stack).
#[inline]
pub(crate) unsafe fn reference(L: *mut lua_State, index: c_int) -> c_int {
    unsafe {
        lua_pushvalue(L, index);
        luaL_ref(L, LUA_REGISTRYINDEX)
    }
}

#[inline]
pub(crate) unsafe fn unref(L: *mut lua_State, reference: c_int) {
    if reference >= 0 {
        unsafe { luaL_unref(L, LUA_REGISTRYINDEX, reference) }
    }
}

/// Bytes of the string at `index`, or `None` for non-strings/non-numbers.
/// The slice is valid while the value stays on the stack.
#[inline]
pub(crate) unsafe fn to_bytes<'a>(L: *mut lua_State, index: c_int) -> Option<&'a [u8]> {
    let mut len = 0usize;
    let data = unsafe { lua_tolstring(L, index, &mut len) };
    if data.is_null() {
        None
    } else {
        Some(unsafe { core::slice::from_raw_parts(data as *const u8, len) })
    }
}

/// Bytes of a string value only (no number conversion, unlike `to_bytes`).
#[inline]
pub(crate) unsafe fn string_bytes<'a>(L: *mut lua_State, index: c_int) -> Option<&'a [u8]> {
    if unsafe { lua_type(L, index) } != LUA_TSTRING {
        return None;
    }
    unsafe { to_bytes(L, index) }
}

/// `luaL_error`: raises `message` prefixed with the caller position.
pub(crate) unsafe fn fail(L: *mut lua_State, message: &[u8]) -> ! {
    unsafe {
        luaL_where(L, 1);
        push_bytes(L, message);
        lua_concat(L, 2);
        lua_error(L)
    }
}

/// `luaL_checklstring` as bytes.
#[inline]
pub(crate) unsafe fn check_bytes<'a>(L: *mut lua_State, arg: c_int) -> &'a [u8] {
    let mut len = 0usize;
    let data = unsafe { luaL_checklstring(L, arg, &mut len) };
    unsafe { core::slice::from_raw_parts(data as *const u8, len) }
}

/// `ac_require`: `package.loaded[name]` if loaded, else `require(name)`.
pub(crate) unsafe fn require(L: *mut lua_State, name: &[u8]) {
    unsafe {
        lua_getfield(L, LUA_REGISTRYINDEX, cstr!("_LOADED"));
        push_bytes(L, name);
        lua_rawget(L, -2);
        if truthy(L, -1) && lua_type(L, -1) != LUA_TLIGHTUSERDATA {
            lua_remove(L, -2);
            return;
        }
        pop(L, 2);
        get_global(L, cstr!("require"));
        push_bytes(L, name);
        lua_call(L, 1, 1);
    }
}

/// `ngx.<table>.<field>` (either part optional).
pub(crate) unsafe fn ngx_member(L: *mut lua_State, table: Option<*const c_char>, field: Option<*const c_char>) {
    unsafe {
        get_global(L, cstr!("ngx"));
        if let Some(table) = table {
            lua_getfield(L, -1, table);
            lua_remove(L, -2);
        }
        if let Some(field) = field {
            lua_getfield(L, -1, field);
            lua_remove(L, -2);
        }
    }
}

/// `t[name] = value` with a byte-string key, value from the top (popped).
#[inline]
pub(crate) unsafe fn set_bytes_field(L: *mut lua_State, table: c_int, name: &[u8]) {
    unsafe {
        let table = absolute(L, table);
        push_bytes(L, name);
        lua_insert(L, -2);
        lua_settable(L, table);
    }
}

/// Pushes a serde_json value as a Lua value; JSON null becomes
/// `cjson.null`-equivalent light userdata NULL, as cjson decoding does.
pub(crate) unsafe fn push_json(L: *mut lua_State, value: &serde_json::Value, depth: usize) {
    use serde_json::Value;
    if depth > 128 {
        unsafe { fail(L, b"native configuration exceeds maximum nesting") }
    }
    unsafe {
        match value {
            Value::Null => lua_pushlightuserdata(L, ptr::null_mut()),
            Value::Bool(flag) => lua_pushboolean(L, *flag as c_int),
            Value::Number(number) => lua_pushnumber(L, number.as_f64().unwrap_or(0.0)),
            Value::String(text) => push_bytes(L, text.as_bytes()),
            Value::Array(items) => {
                lua_createtable(L, items.len() as c_int, 0);
                for (index, item) in items.iter().enumerate() {
                    push_json(L, item, depth + 1);
                    lua_rawseti(L, -2, index as c_int + 1);
                }
            }
            Value::Object(map) => {
                lua_createtable(L, 0, map.len() as c_int);
                for (key, item) in map {
                    push_bytes(L, key.as_bytes());
                    push_json(L, item, depth + 1);
                    lua_rawset(L, -3);
                }
            }
        }
    }
}
