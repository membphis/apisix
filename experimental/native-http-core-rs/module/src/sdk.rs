// SPDX-License-Identifier: Apache-2.0
//! Step 1: the APISIX SDK subset the unchanged Lua key-auth plugin uses,
//! implemented in Rust through the Lua C API. Each function ports the C
//! prototype's V1 implementation (`module/sdk.c`, `request.c`, `response.c`,
//! `consumer.c`, `schema.c`) with the six selected switches of the measured
//! C variant (batched variable writes, request-header flags, header scratch
//! buffer, native api_ctx table pool, lazy route options, lazy rate JSON).
//!
//! Provided modules: `apisix.core` (lazy members), `apisix.core.version`,
//! `apisix.core.log`, `apisix.core.request`, `apisix.core.response`,
//! `apisix.core.schema`, `apisix.core.ctx`, `apisix.schema_def`,
//! `apisix.consumer`, `apisix.utils.auth`, `apisix.tracer`. Members outside
//! this subset raise an explicit error instead of silently returning nil.

#![allow(non_snake_case)] // `L` is the Lua C API's name for the state.

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use ngx::ffi::*;

use crate::lua::*;
use crate::step1::{self, engine};

const SCHEMA_DEF: &str = r##"{
  "anonymous_consumer_schema": {"type": "string", "minLength": 1},
  "plugin_injected_schema": {
    "$comment": "this is a mark for our injected plugin schema",
    "_meta": {
      "type": "object",
      "properties": {
        "disable": {"type": "boolean"},
        "error_response": {"oneOf": [{"type": "string"}, {"type": "object"}]},
        "priority": {"description": "priority of plugins by customized order", "type": "integer"},
        "filter": {"description": "filter determines whether the plugin needs to be executed at runtime", "type": "array"},
        "pre_function": {"description": "function to be executed in each phase before execution of plugins. The pre_function will have access to two arguments: `conf` and `ctx`.", "type": "string"}
      },
      "additionalProperties": false
    }
  }
}"##;

pub(crate) const MODULES: &[(&[u8], lua_CFunction)] = &[
    (b"apisix.native.engine\0", step1::engine_open),
    (b"apisix.core\0", core_open),
    (b"apisix.core.version\0", version_open),
    (b"apisix.core.log\0", log_open),
    (b"apisix.core.request\0", request_open),
    (b"apisix.core.response\0", response_open),
    (b"apisix.core.schema\0", schema_open),
    (b"apisix.core.ctx\0", ctx_open),
    (b"apisix.schema_def\0", schema_def_open),
    (b"apisix.consumer\0", consumer_open),
    (b"apisix.utils.auth\0", auth_open),
    (b"apisix.tracer\0", tracer_open),
];

unsafe fn register(L: *mut lua_State, functions: &[(&[u8], lua_CFunction)]) {
    for (name, function) in functions {
        unsafe {
            push_fn(L, *function);
            lua_setfield(L, -2, name.as_ptr() as *const c_char);
        }
    }
}

// ---------------------------------------------------------------- apisix.core

unsafe extern "C-unwind" fn core_index(L: *mut lua_State) -> c_int {
    const MEMBERS: &[(&[u8], &[u8])] = &[
        (b"version", b"apisix.core.version"),
        (b"log", b"apisix.core.log"),
        (b"request", b"apisix.core.request"),
        (b"response", b"apisix.core.response"),
        (b"schema", b"apisix.core.schema"),
        (b"ctx", b"apisix.core.ctx"),
    ];
    unsafe {
        let field = check_bytes(L, 2);
        let Some((_, module)) = MEMBERS.iter().find(|(name, _)| *name == field) else {
            let mut message = b"apisix.core.".to_vec();
            message.extend_from_slice(field);
            message.extend_from_slice(b" is not provided by the Rust step-1 core");
            fail(L, &message)
        };
        require(L, module);
        lua_pushvalue(L, 2);
        lua_pushvalue(L, -2);
        lua_rawset(L, 1);
        1
    }
}

unsafe extern "C-unwind" fn core_open(L: *mut lua_State) -> c_int {
    unsafe {
        new_table(L);
        new_table(L);
        push_fn(L, core_index);
        lua_setfield(L, -2, cstr!("__index"));
        lua_setmetatable(L, -2);
        1
    }
}

unsafe extern "C-unwind" fn version_open(L: *mut lua_State) -> c_int {
    unsafe {
        new_table(L);
        push_bytes(L, b"3.19.0");
        lua_setfield(L, -2, cstr!("VERSION"));
        1
    }
}

// ------------------------------------------------------------ apisix.core.log

/// `ac_log`: `ngx.log(level, [prefix,] ...)`.
unsafe extern "C-unwind" fn log(L: *mut lua_State) -> c_int {
    unsafe {
        let mut count = lua_gettop(L);
        ngx_member(L, None, Some(cstr!("log")));
        lua_insert(L, 1);
        lua_pushvalue(L, upvalue(1));
        lua_insert(L, 2);
        if !is_nil(L, upvalue(2)) {
            lua_pushvalue(L, upvalue(2));
            lua_insert(L, 3);
            count += 1;
        }
        lua_call(L, count + 1, 0);
        0
    }
}

unsafe extern "C-unwind" fn delay_tostring(L: *mut lua_State) -> c_int {
    unsafe {
        lua_getfield(L, 1, cstr!("func"));
        lua_getfield(L, 1, cstr!("args"));
        let count = lua_objlen(L, -1) as c_int;
        for index in 1..=count {
            lua_rawgeti(L, 3, index);
        }
        lua_remove(L, 3);
        lua_call(L, count, 1);
        get_global(L, cstr!("tostring"));
        lua_insert(L, -2);
        lua_call(L, 1, 1);
        1
    }
}

unsafe extern "C-unwind" fn delay_exec(L: *mut lua_State) -> c_int {
    unsafe {
        let count = lua_gettop(L);
        luaL_checktype(L, 1, LUA_TFUNCTION);
        new_table(L);
        let result = lua_gettop(L);
        lua_pushvalue(L, 1);
        lua_setfield(L, result, cstr!("func"));
        lua_createtable(L, count - 1, 0);
        for index in 2..=count {
            lua_pushvalue(L, index);
            lua_rawseti(L, -2, index - 1);
        }
        lua_setfield(L, result, cstr!("args"));
        new_table(L);
        push_fn(L, delay_tostring);
        lua_setfield(L, -2, cstr!("__tostring"));
        lua_setmetatable(L, result);
        1
    }
}

unsafe fn log_table(L: *mut lua_State, prefix: c_int) {
    const LEVELS: [(&[u8], isize); 9] = [
        (b"stderr\0", 0),
        (b"emerg\0", 1),
        (b"alert\0", 2),
        (b"crit\0", 3),
        (b"error\0", 4),
        (b"warn\0", 5),
        (b"notice\0", 6),
        (b"info\0", 7),
        (b"debug\0", 8),
    ];
    unsafe {
        new_table(L);
        for (name, level) in LEVELS {
            lua_pushinteger(L, level);
            if prefix != 0 {
                lua_pushvalue(L, prefix);
            } else {
                lua_pushnil(L);
            }
            lua_pushcclosure(L, log, 2);
            lua_setfield(L, -2, name.as_ptr() as *const c_char);
        }
        push_fn(L, log_new);
        lua_setfield(L, -2, cstr!("new"));
        push_fn(L, delay_exec);
        lua_setfield(L, -2, cstr!("delay_exec"));
    }
}

unsafe extern "C-unwind" fn log_new(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 1);
        log_table(L, 1);
        1
    }
}

unsafe extern "C-unwind" fn log_open(L: *mut lua_State) -> c_int {
    unsafe { log_table(L, 0) };
    1
}

// -------------------------------------------------------- apisix.core.request

/// `ac_arg_context`: the explicit ctx argument, or the request's api_ctx.
unsafe fn arg_context(L: *mut lua_State, index: c_int) -> c_int {
    unsafe {
        if is_none_or_nil(L, index) {
            let ctx = step1::context(L);
            push_ref(L, step1::table_ref(ctx));
        } else {
            lua_pushvalue(L, index);
        }
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_gettop(L)
    }
}

#[inline]
unsafe fn false_to_nil(L: *mut lua_State, index: c_int) {
    unsafe {
        if lua_type(L, index) == LUA_TBOOLEAN && !truthy(L, index) {
            lua_pushnil(L);
            lua_replace(L, index);
        }
    }
}

/// `ac_headers_for_context`: the cached `ngx.req.get_headers()` table,
/// invalidated by the apisix-nginx-module "request header set" flag.
unsafe fn headers_for_context(L: *mut lua_State, ctx: c_int) -> c_int {
    unsafe {
        let r = ngx_http_lua_get_request(L);
        if !r.is_null() && ngx_http_apisix_is_request_header_set(r) == 1 {
            ngx_http_apisix_clear_request_header(r);
            lua_pushnil(L);
            lua_setfield(L, ctx, cstr!("headers"));
        }
        lua_getfield(L, ctx, cstr!("headers"));
        if !truthy(L, -1) {
            pop(L, 1);
            ngx_member(L, Some(cstr!("req")), Some(cstr!("get_headers")));
            lua_call(L, 0, 1);
            lua_pushvalue(L, -1);
            lua_setfield(L, ctx, cstr!("headers"));
        }
        1
    }
}

unsafe extern "C-unwind" fn headers(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 1);
        false_to_nil(L, 1);
        headers_for_context(L, arg_context(L, 1))
    }
}

unsafe extern "C-unwind" fn header(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 2);
        false_to_nil(L, 1);
        // sdk_cache_replay is off in the measured C variant: a nested call.
        push_fn(L, headers);
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        lua_pushvalue(L, 2);
        lua_gettable(L, -2);
        if is_table(L, -1) {
            // `type(value) == "table" and value[1] or value`
            lua_pushinteger(L, 1);
            lua_gettable(L, -2);
            if !truthy(L, -1) {
                pop(L, 1);
            }
        }
        1
    }
}

/// `ac_native_header_write` for a Lua-engine request (not the C pipeline):
/// ngx_lua's exported setters do the validation and header bookkeeping.
pub(crate) unsafe fn native_header_write(
    r: *mut ngx_http_request_t,
    name: &[u8],
    value: Option<&[u8]>,
    response: bool,
    append: bool,
) -> Result<(), &'static str> {
    if append && value.is_none() {
        return Err("invalid header value");
    }
    let mut error: *mut c_char = ptr::null_mut();
    let (data, len) = value.map_or((ptr::null(), 0), |value| (value.as_ptr(), value.len()));
    let status = unsafe {
        if response {
            ngx_http_lua_ffi_set_resp_header(
                r,
                name.as_ptr() as *const c_char,
                name.len(),
                value.is_none() as c_int,
                data as *const c_char,
                len,
                ptr::null_mut(),
                0,
                (!append) as c_int,
                &mut error,
            )
        } else {
            crate::http::req_set_header(r, name, data, len, (!append) as c_int, &mut error)
        }
    };
    if status == NGX_OK as c_int || status == NGX_DECLINED as c_int {
        Ok(())
    } else if error.is_null() {
        Err("invalid request context")
    } else {
        Err("header update rejected")
    }
}

pub(crate) unsafe fn native_header_set(
    L: *mut lua_State,
    r: *mut ngx_http_request_t,
    name: &[u8],
    value: Option<&[u8]>,
    response: bool,
    append: bool,
) {
    if let Err(error) = unsafe { native_header_write(r, name, value, response, append) } {
        let mut message = b"native header update failed: ".to_vec();
        message.extend_from_slice(error.as_bytes());
        unsafe { fail(L, &message) }
    }
}

/// `ac_modify_header`: request header set/add, plus the `ctx.var` and
/// `ctx.headers` cache maintenance of the SDK.
unsafe extern "C-unwind" fn modify_header(L: *mut lua_State) -> c_int {
    unsafe {
        let append = truthy(L, upvalue(1));
        let r = ngx_http_lua_get_request(L);
        lua_settop(L, 3);
        let (ctx, name, value) = if lua_type(L, 1) == LUA_TSTRING {
            let ctx = step1::context(L);
            push_ref(L, step1::table_ref(ctx));
            (lua_gettop(L), 1, 2)
        } else {
            (arg_context(L, 1), 2, 3)
        };
        let Some(raw) = string_bytes(L, name) else { fail(L, b"invalid header name: expected string") };
        // header_scratch: a stack buffer for short cache keys.
        let mut scratch = [0u8; 128];
        let mut heap;
        let lower: &mut [u8] = if raw.len() + 6 <= scratch.len() {
            &mut scratch[..raw.len() + 5]
        } else {
            heap = vec![0u8; raw.len() + 5];
            &mut heap[..]
        };
        lower[..5].copy_from_slice(b"http_");
        for (out, byte) in lower[5..].iter_mut().zip(raw) {
            *out = byte.to_ascii_lowercase();
        }
        let changed = !r.is_null() && ngx_http_apisix_is_request_header_set(r) == 1;
        if is_nil(L, value) || lua_isstring(L, value) != 0 {
            let field = if is_nil(L, value) {
                None
            } else {
                lua_pushvalue(L, value);
                to_bytes(L, -1)
            };
            native_header_set(L, r, raw, field, false, append);
            if field.is_some() {
                pop(L, 1);
            }
        } else if append {
            require(L, b"ngx.req");
            lua_getfield(L, -1, cstr!("add_header"));
            lua_remove(L, -2);
            lua_pushvalue(L, name);
            lua_pushvalue(L, value);
            lua_call(L, 2, 0);
        } else {
            ngx_member(L, Some(cstr!("req")), Some(cstr!("set_header")));
            lua_pushvalue(L, name);
            lua_pushvalue(L, value);
            lua_call(L, 2, 0);
        }
        lua_getfield(L, ctx, cstr!("var"));
        if is_table(L, -1) {
            let mut key = lower.to_vec();
            for byte in &mut key[5..] {
                if *byte == b'-' {
                    *byte = b'_';
                }
            }
            push_bytes(L, &key);
            lua_pushnil(L);
            lua_settable(L, -3);
        }
        pop(L, 1);
        if !changed {
            if !r.is_null() {
                ngx_http_apisix_clear_request_header(r);
            }
            lua_getfield(L, ctx, cstr!("headers"));
            let cache = lua_gettop(L);
            if is_table(L, cache) {
                push_bytes(L, &lower[5..]);
                lua_rawget(L, cache);
                if !append || !truthy(L, -1) {
                    push_bytes(L, &lower[5..]);
                    lua_pushvalue(L, value);
                    lua_rawset(L, cache);
                } else if is_table(L, -1) {
                    lua_pushvalue(L, value);
                    let next = lua_objlen(L, -2) as c_int + 1;
                    lua_rawseti(L, -2, next);
                } else {
                    lua_createtable(L, 2, 0);
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, -2, 1);
                    lua_pushvalue(L, value);
                    lua_rawseti(L, -2, 2);
                    push_bytes(L, &lower[5..]);
                    lua_pushvalue(L, -2);
                    lua_rawset(L, cache);
                    pop(L, 1);
                }
                pop(L, 1);
            }
            pop(L, 1);
        }
        0
    }
}

pub(crate) unsafe extern "C-unwind" fn uri_args(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 1);
        false_to_nil(L, 1);
        let ctx = arg_context(L, 1);
        lua_getfield(L, ctx, cstr!("req_uri_args"));
        if !truthy(L, -1) {
            pop(L, 1);
            ngx_member(L, Some(cstr!("req")), Some(cstr!("get_uri_args")));
            lua_pushinteger(L, 0);
            lua_call(L, 1, 1);
            lua_pushvalue(L, -1);
            lua_setfield(L, ctx, cstr!("req_uri_args"));
        }
        1
    }
}

unsafe extern "C-unwind" fn set_uri_args(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 2);
        false_to_nil(L, 1);
        let ctx = arg_context(L, 1);
        lua_pushnil(L);
        lua_setfield(L, ctx, cstr!("req_uri_args"));
        ngx_member(L, Some(cstr!("req")), Some(cstr!("set_uri_args")));
        lua_pushvalue(L, 2);
        lua_call(L, 1, LUA_MULTRET);
        lua_gettop(L) - ctx
    }
}

/// `ac_request_var`: `get_ip`, `get_remote_client_ip`, ... over `ctx.var`.
unsafe extern "C-unwind" fn request_var(L: *mut lua_State) -> c_int {
    unsafe {
        let numeric = truthy(L, upvalue(2));
        lua_settop(L, 1);
        false_to_nil(L, 1);
        let ctx = arg_context(L, 1);
        lua_getfield(L, ctx, cstr!("var"));
        let name = to_bytes(L, upvalue(1)).unwrap_or_default();
        if name == b"realip_remote_addr" {
            lua_getfield(L, -1, cstr!("realip_remote_addr"));
            if !truthy(L, -1) {
                pop(L, 1);
                lua_getfield(L, -1, cstr!("remote_addr"));
            }
        } else {
            lua_pushvalue(L, upvalue(1));
            lua_gettable(L, -2);
        }
        if numeric {
            if lua_isnumber(L, -1) != 0 {
                lua_pushnumber(L, lua_tonumber(L, -1));
            } else {
                lua_pushnil(L);
            }
        } else if !truthy(L, -1) {
            push_bytes(L, b"");
        }
        1
    }
}

unsafe extern "C-unwind" fn request_open(L: *mut lua_State) -> c_int {
    const VARS: [(&[u8], &[u8], bool); 6] = [
        (b"get_ip\0", b"realip_remote_addr", false),
        (b"get_remote_client_ip\0", b"remote_addr", false),
        (b"get_remote_client_port\0", b"remote_port", true),
        (b"get_scheme\0", b"scheme", false),
        (b"get_host\0", b"host", false),
        (b"get_port\0", b"server_port", true),
    ];
    unsafe {
        new_table(L);
        register(
            L,
            &[
                (b"headers\0", headers),
                (b"header\0", header),
                (b"get_uri_args\0", uri_args),
                (b"set_uri_args\0", set_uri_args),
            ],
        );
        for (append, name) in [(false, cstr!("set_header")), (true, cstr!("add_header"))] {
            lua_pushboolean(L, append as c_int);
            lua_pushcclosure(L, modify_header, 1);
            lua_setfield(L, -2, name);
        }
        for (method, name, numeric) in VARS {
            push_bytes(L, name);
            lua_pushboolean(L, numeric as c_int);
            lua_pushcclosure(L, request_var, 2);
            lua_setfield(L, -2, method.as_ptr() as *const c_char);
        }
        1
    }
}

// ------------------------------------------------------- apisix.core.response

/// `ac_set_header` (response): native fast path for string/nil pairs.
unsafe extern "C-unwind" fn response_set_header(L: *mut lua_State) -> c_int {
    unsafe {
        let count = lua_gettop(L);
        let append = truthy(L, upvalue(1));
        if count % 2 != 0 {
            fail(L, b"invalid argument count, expected header/value pairs");
        }
        if !append {
            let plain = (1..=count).step_by(2).all(|index| {
                lua_type(L, index) == LUA_TSTRING && (is_nil(L, index + 1) || lua_isstring(L, index + 1) != 0)
            });
            if plain {
                let r = ngx_http_lua_get_request(L);
                if r.is_null() {
                    fail(L, b"no request found");
                }
                for index in (1..=count).step_by(2) {
                    let name = to_bytes(L, index).unwrap_or_default();
                    let field = if is_nil(L, index + 1) {
                        None
                    } else {
                        lua_pushvalue(L, index + 1);
                        to_bytes(L, -1)
                    };
                    native_header_set(L, r, name, field, true, false);
                    if field.is_some() {
                        pop(L, 1);
                    }
                }
                return 0;
            }
        }
        ngx_member(L, Some(cstr!("header")), None);
        let table = count + 1;
        for index in (1..=count).step_by(2) {
            lua_pushvalue(L, index);
            let mut value = index + 1;
            if append {
                lua_gettable(L, table);
                if is_nil(L, -1) {
                    pop(L, 1);
                } else {
                    if !is_table(L, -1) {
                        lua_createtable(L, 2, 0);
                        lua_pushvalue(L, -2);
                        lua_rawseti(L, -2, 1);
                        lua_remove(L, -2);
                    }
                    let next = lua_objlen(L, -1) as c_int + 1;
                    lua_pushvalue(L, value);
                    lua_rawseti(L, -2, next);
                    value = lua_gettop(L);
                }
                lua_pushvalue(L, index);
            }
            lua_pushvalue(L, value);
            lua_settable(L, table);
            lua_settop(L, table);
        }
        0
    }
}

unsafe extern "C-unwind" fn response_open(L: *mut lua_State) -> c_int {
    unsafe {
        new_table(L);
        for (append, name) in [(false, cstr!("set_header")), (true, cstr!("add_header"))] {
            lua_pushboolean(L, append as c_int);
            lua_pushcclosure(L, response_set_header, 1);
            lua_setfield(L, -2, name);
        }
        1
    }
}

// --------------------------------------------- apisix.schema_def / core.schema

unsafe extern "C-unwind" fn realm_schema(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 1);
        lua_createtable(L, 0, 5);
        push_bytes(L, b"string");
        lua_setfield(L, -2, cstr!("type"));
        push_bytes(L, b"^[\x20-\x21\x23-\x5B\\]-\x7E]+$");
        lua_setfield(L, -2, cstr!("pattern"));
        lua_pushvalue(L, 1);
        lua_setfield(L, -2, cstr!("default"));
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, cstr!("minLength"));
        lua_pushinteger(L, 128);
        lua_setfield(L, -2, cstr!("maxLength"));
        1
    }
}

unsafe extern "C-unwind" fn schema_def_open(L: *mut lua_State) -> c_int {
    let value: serde_json::Value = match serde_json::from_str(SCHEMA_DEF) {
        Ok(value) => value,
        Err(_) => unsafe { fail(L, b"invalid built-in schema definitions") },
    };
    unsafe {
        push_json(L, &value, 0);
        push_fn(L, realm_schema);
        lua_setfield(L, -2, cstr!("get_realm_schema"));
        1
    }
}

/// `skip_validation`: secret references are not validated as literals.
unsafe extern "C-unwind" fn secret_ref(L: *mut lua_State) -> c_int {
    unsafe {
        let Some(value) = string_bytes(L, 1) else {
            lua_pushboolean(L, 0);
            return 1;
        };
        if is_table(L, 2) {
            lua_getfield(L, 2, cstr!("type"));
            let kind = to_bytes(L, -1);
            if !is_nil(L, -1) && (lua_isstring(L, -1) == 0 || kind != Some(b"string".as_slice())) {
                lua_pushboolean(L, 0);
                return 1;
            }
            pop(L, 1);
        }
        let is_ref = value.starts_with(b"$secret://") || value.len() >= 7 && value[..7].eq_ignore_ascii_case(b"$ENV://");
        lua_pushboolean(L, is_ref as c_int);
        1
    }
}

/// `ac_schema_validator`: cached `jsonschema.generate_validator`.
unsafe extern "C-unwind" fn schema_validator(L: *mut lua_State) -> c_int {
    unsafe {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_getfield(L, upvalue(1), cstr!("get"));
        lua_pushvalue(L, upvalue(1));
        lua_pushvalue(L, 1);
        lua_call(L, 2, 1);
        if !is_nil(L, -1) {
            lua_pushnil(L);
            return 2;
        }
        pop(L, 1);
        require(L, b"jsonschema");
        lua_getfield(L, -1, cstr!("generate_validator"));
        lua_pushvalue(L, 1);
        new_table(L);
        let options = lua_gettop(L);
        push_fn(L, secret_ref);
        lua_setfield(L, options, cstr!("skip_validation"));
        if lua_pcall(L, 2, 1, 0) != 0 {
            lua_pushnil(L);
            lua_insert(L, -2);
            return 2;
        }
        let cached = lua_gettop(L);
        lua_getfield(L, upvalue(1), cstr!("set"));
        lua_pushvalue(L, upvalue(1));
        lua_pushvalue(L, 1);
        lua_pushvalue(L, cached);
        lua_pushinteger(L, 0);
        lua_call(L, 4, 0);
        lua_pushnil(L);
        2
    }
}

unsafe extern "C-unwind" fn schema_check(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 2);
        lua_pushvalue(L, upvalue(1));
        lua_pushvalue(L, 1);
        lua_call(L, 1, 2);
        if !is_function(L, -2) {
            lua_pushboolean(L, 0);
            lua_replace(L, -3);
            return 2;
        }
        pop(L, 1);
        lua_pushvalue(L, 2);
        lua_call(L, 1, LUA_MULTRET);
        lua_gettop(L) - 2
    }
}

unsafe extern "C-unwind" fn schema_readonly(L: *mut lua_State) -> c_int {
    unsafe { fail(L, b"no modification allowed") }
}

unsafe extern "C-unwind" fn schema_open(L: *mut lua_State) -> c_int {
    unsafe {
        new_table(L);
        let module = lua_gettop(L);
        lua_pushinteger(L, 1);
        lua_setfield(L, module, cstr!("TYPE_CONSUMER"));
        lua_pushinteger(L, 2);
        lua_setfield(L, module, cstr!("TYPE_METADATA"));
        require(L, b"resty.lrucache");
        lua_getfield(L, -1, cstr!("new"));
        lua_pushinteger(L, 1000);
        lua_call(L, 1, 1);
        lua_remove(L, -2);
        lua_pushcclosure(L, schema_validator, 1);
        lua_pushvalue(L, -1);
        lua_setfield(L, module, cstr!("valid"));
        lua_pushcclosure(L, schema_check, 1);
        lua_setfield(L, module, cstr!("check"));
        new_table(L);
        require(L, b"apisix.schema_def");
        lua_setfield(L, -2, cstr!("__index"));
        push_fn(L, schema_readonly);
        lua_setfield(L, -2, cstr!("__newindex"));
        lua_setmetatable(L, module);
        1
    }
}

// ----------------------------------------------------------- apisix.core.ctx

/// `ac_done`: a completed bound call returns `nil, values...`.
unsafe fn done(L: *mut lua_State, count: c_int) -> c_int {
    unsafe {
        lua_pushnil(L);
        lua_insert(L, lua_gettop(L) - count);
    }
    count + 1
}

/// `ac_io_call`: hands `callable(args...)` to the coroutine trampoline,
/// resuming in `resume` (with `state` as its upvalue).
unsafe fn io_call(L: *mut lua_State, state: c_int, resume: lua_CFunction, callable: c_int, args: c_int, count: c_int) -> c_int {
    unsafe {
        let state = absolute(L, state);
        let callable = absolute(L, callable);
        let args = absolute(L, args);
        lua_pushvalue(L, state);
        lua_pushcclosure(L, resume, 1);
        lua_pushvalue(L, callable);
        lua_createtable(L, count, 1);
        for index in 0..count {
            lua_pushvalue(L, args + index);
            lua_rawseti(L, -2, index + 1);
        }
        lua_pushinteger(L, count as isize);
        lua_setfield(L, -2, cstr!("n"));
        3
    }
}

unsafe fn var_cache(L: *mut lua_State, var: c_int, key: c_int, value: c_int) {
    unsafe {
        let (var, key, value) = (absolute(L, var), absolute(L, key), absolute(L, value));
        let name = to_bytes(L, key).unwrap_or_default();
        if name == b"args" || name == b"is_args" {
            return;
        }
        lua_getfield(L, var, cstr!("_cache"));
        lua_pushvalue(L, key);
        lua_pushvalue(L, value);
        lua_rawset(L, -3);
        pop(L, 1);
    }
}

unsafe extern "C-unwind" fn var_resume(L: *mut lua_State) -> c_int {
    unsafe {
        let state = upvalue(1);
        if !truthy(L, 1) {
            lua_pushvalue(L, 2);
            lua_error(L);
        }
        lua_settop(L, 2);
        lua_getfield(L, state, cstr!("uncached"));
        let uncached = truthy(L, -1);
        pop(L, 1);
        if !uncached {
            lua_getfield(L, state, cstr!("var"));
            lua_getfield(L, state, cstr!("key"));
            var_cache(L, -2, -1, 2);
            pop(L, 2);
        }
        lua_pushnil(L);
        lua_pushvalue(L, 2);
        2
    }
}

/// `ac_native_var_fetch`: typed reads of common variables, else the NGINX
/// variable of the (lower-cased) name.
unsafe fn native_var_fetch(L: *mut lua_State, r: *mut ngx_http_request_t, key: &[u8]) {
    unsafe {
        let mut value = ngx_str_t { len: 0, data: ptr::null_mut() };
        match key {
            b"status" => return lua_pushinteger(L, ngx_http_lua_ffi_get_resp_status(r) as isize),
            b"request_method" | b"method" => return push_str(L, &(*r).method_name),
            b"uri" => {
                ngx_http_lua_var_ffi_uri(r, &mut value);
            }
            b"host" => {
                ngx_http_lua_var_ffi_host(r, &mut value);
            }
            b"remote_addr" => {
                ngx_http_lua_var_ffi_remote_addr(r, &mut value);
            }
            b"scheme" => {
                ngx_http_lua_var_ffi_scheme(r, &mut value);
            }
            b"upstream_response_time" | b"upstream_header_time" | b"upstream_connect_time" | b"request_time" => {
                fail(L, b"timing variables are not provided by the Rust step-1 core")
            }
            _ => {
                let mut lower = key.to_ascii_lowercase();
                if key.starts_with(b"http_") {
                    for byte in &mut lower {
                        if *byte == b'-' {
                            *byte = b'_';
                        }
                    }
                }
                let hash = lower.iter().fold(0 as ngx_uint_t, |h, &b| h.wrapping_mul(31).wrapping_add(b as ngx_uint_t));
                let mut name = ngx_str_t { len: lower.len(), data: lower.as_mut_ptr() };
                let variable = ngx_http_get_variable(r, &mut name, hash);
                if !variable.is_null() && (*variable).not_found() == 0 {
                    value = ngx_str_t { len: (*variable).len() as usize, data: (*variable).data };
                }
                if key == b"request_length" || key == b"bytes_sent" {
                    let number = if value.data.is_null() { NGX_ERROR as off_t } else { ngx_atoof(value.data, value.len) };
                    if number == NGX_ERROR as off_t {
                        lua_pushnil(L);
                    } else {
                        lua_pushnumber(L, number as f64);
                    }
                    return;
                }
            }
        }
        if value.data.is_null() {
            lua_pushnil(L);
        } else {
            push_str(L, &value);
        }
    }
}

/// `ac_var_index` (bound through `runtime.bind`, behind `runtime.cached_index`).
unsafe extern "C-unwind" fn var_index(L: *mut lua_State) -> c_int {
    const BUILTIN: [&[u8]; 10] = [
        b"balancer_ip",
        b"balancer_port",
        b"consumer_group_id",
        b"consumer_name",
        b"resp_body",
        b"route_id",
        b"route_name",
        b"service_id",
        b"service_name",
        b"upstream_unresolved_host",
    ];
    unsafe {
        lua_settop(L, 2);
        if lua_type(L, 2) != LUA_TSTRING {
            fail(L, b"invalid argument, expect string value");
        }
        let key = to_bytes(L, 2).unwrap_or_default();
        lua_getfield(L, 1, cstr!("_cache"));
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        if !is_nil(L, -1) {
            return done(L, 1);
        }
        pop(L, 2);
        lua_getfield(L, 1, cstr!("_ctx"));
        let ctx = lua_gettop(L);
        push_ref(L, (*engine()).var_getters_ref);
        let getters = lua_gettop(L);
        lua_pushvalue(L, 2);
        lua_rawget(L, getters);
        let entry = lua_gettop(L);
        if is_table(L, entry) {
            new_table(L);
            let state = lua_gettop(L);
            lua_pushvalue(L, 1);
            lua_setfield(L, state, cstr!("var"));
            lua_pushvalue(L, 2);
            lua_setfield(L, state, cstr!("key"));
            lua_getfield(L, entry, cstr!("getter"));
            let callable = lua_gettop(L);
            lua_getfield(L, entry, cstr!("uncached"));
            lua_setfield(L, state, cstr!("uncached"));
            lua_pushvalue(L, ctx);
            return io_call(L, state, var_resume, callable, -1, 1);
        }
        if key.starts_with(b"post_arg_") {
            fail(L, b"post_arg_* variables are not provided by the Rust step-1 core");
        }
        pop(L, 2);
        let r = ngx_http_lua_get_request(L);
        if key == b"method" {
            push_str(L, &(*r).method_name);
        } else if let Some(name) = key.strip_prefix(b"arg_") {
            push_fn(L, uri_args);
            lua_pushvalue(L, ctx);
            lua_call(L, 1, 1);
            push_bytes(L, name);
            lua_gettable(L, -2);
            if is_table(L, -1) {
                lua_rawgeti(L, -1, 1);
            }
        } else if let Some(name) = key.strip_prefix(b"uri_param_") {
            lua_getfield(L, ctx, cstr!("curr_req_matched"));
            if is_table(L, -1) {
                push_bytes(L, name);
                lua_gettable(L, -2);
            }
        } else if key == b"cookie" || key.starts_with(b"cookie_") {
            require(L, b"resty.cookie");
            lua_getfield(L, -1, cstr!("new"));
            lua_pushvalue(L, -2);
            lua_call(L, 1, 1);
            if let Some(name) = key.strip_prefix(b"cookie_") {
                if is_table(L, -1) {
                    lua_getfield(L, -1, cstr!("get"));
                    lua_pushvalue(L, -2);
                    push_bytes(L, name);
                    lua_call(L, 2, 1);
                }
            }
        } else if BUILTIN.contains(&key) {
            lua_pushvalue(L, 2);
            lua_gettable(L, ctx);
            if is_nil(L, -1) && key == b"resp_body" {
                push_bytes(L, b"");
            }
        } else {
            native_var_fetch(L, r, key);
        }
        var_cache(L, 1, 2, -1);
        done(L, 1)
    }
}

/// Variables C's `ac_var_newindex` writes through, limited to the ones this
/// module registers; other C-writable names are rejected explicitly.
const WRITABLE: [&[u8]; 6] = [
    b"upstream_scheme",
    b"upstream_host",
    b"upstream_unresolved_host",
    b"upstream_uri",
    b"request_line",
    b"original_x_forwarded_for",
];
const C_ONLY_WRITABLE: [&[u8]; 28] = [
    b"upstream_upgrade",
    b"upstream_connection",
    b"llm_content_risk_level",
    b"apisix_request_id",
    b"request_type",
    b"apisix_upstream_response_time",
    b"llm_time_to_first_token",
    b"request_llm_model",
    b"llm_model",
    b"llm_prompt_tokens",
    b"llm_completion_tokens",
    b"llm_total_tokens",
    b"llm_stream",
    b"llm_has_tool_calls",
    b"llm_tool_count",
    b"llm_end_user_id",
    b"llm_cache_read_input_tokens",
    b"llm_cache_creation_input_tokens",
    b"llm_reasoning_tokens",
    b"upstream_mirror_host",
    b"upstream_mirror_uri",
    b"upstream_mirror_grpc_path",
    b"upstream_cache_zone",
    b"upstream_cache_zone_info",
    b"upstream_no_cache",
    b"upstream_cache_key",
    b"upstream_cache_bypass",
    b"rate_limiting_info",
];

unsafe extern "C-unwind" fn var_newindex(L: *mut lua_State) -> c_int {
    unsafe {
        let key = check_bytes(L, 2);
        lua_settop(L, 3);
        if C_ONLY_WRITABLE.contains(&key) {
            fail(L, b"this variable is not provided by the Rust step-1 core");
        }
        if WRITABLE.contains(&key) {
            let value = if is_nil(L, 3) {
                None
            } else {
                if lua_isstring(L, 3) == 0 {
                    get_global(L, cstr!("tostring"));
                    lua_pushvalue(L, 3);
                    lua_call(L, 1, 1);
                } else {
                    lua_pushvalue(L, 3);
                }
                to_bytes(L, -1)
            };
            step1::native_var_set(L, ngx_http_lua_get_request(L), key, value);
        }
        lua_getfield(L, 1, cstr!("_cache"));
        lua_pushvalue(L, 2);
        lua_pushvalue(L, 3);
        lua_rawset(L, -3);
        0
    }
}

unsafe extern "C-unwind" fn register_var(L: *mut lua_State) -> c_int {
    unsafe {
        check_bytes(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_settop(L, 3);
        push_ref(L, (*engine()).var_getters_ref);
        lua_pushvalue(L, 1);
        new_table(L);
        let entry = lua_gettop(L);
        lua_pushvalue(L, 2);
        lua_setfield(L, entry, cstr!("getter"));
        if is_table(L, 3) {
            lua_getfield(L, 3, cstr!("no_cacheable"));
            lua_setfield(L, entry, cstr!("uncached"));
        }
        lua_rawset(L, -3);
        0
    }
}

/// `ac_set_vars`: pooled `ctx.var` view with the native metatable.
pub(crate) unsafe extern "C-unwind" fn set_vars_meta(L: *mut lua_State) -> c_int {
    unsafe {
        luaL_checktype(L, 1, LUA_TTABLE);
        require(L, b"tablepool");
        lua_getfield(L, -1, cstr!("fetch"));
        push_bytes(L, b"ctx_var");
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 32);
        lua_call(L, 3, 1);
        let var = lua_gettop(L);
        lua_getfield(L, var, cstr!("_cache"));
        if !truthy(L, -1) {
            pop(L, 1);
            new_table(L);
            lua_setfield(L, var, cstr!("_cache"));
        } else {
            pop(L, 1);
        }
        lua_pushvalue(L, 1);
        lua_setfield(L, var, cstr!("_ctx"));
        require(L, b"resty.ngxvar");
        lua_getfield(L, -1, cstr!("request"));
        lua_call(L, 0, 1);
        lua_setfield(L, var, cstr!("_request"));
        pop(L, 1);
        push_ref(L, (*engine()).var_meta_ref);
        lua_setmetatable(L, var);
        lua_pushvalue(L, var);
        lua_setfield(L, 1, cstr!("var"));
        0
    }
}

pub(crate) unsafe extern "C-unwind" fn release_vars(L: *mut lua_State) -> c_int {
    unsafe {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_getfield(L, 1, cstr!("var"));
        if is_nil(L, -1) {
            return 0;
        }
        require(L, b"table.clear");
        lua_getfield(L, 2, cstr!("_cache"));
        lua_call(L, 1, 0);
        require(L, b"tablepool");
        lua_getfield(L, -1, cstr!("release"));
        push_bytes(L, b"ctx_var");
        lua_pushvalue(L, 2);
        lua_pushboolean(L, 1);
        lua_call(L, 3, 0);
        lua_pushnil(L);
        lua_setfield(L, 1, cstr!("var"));
        0
    }
}

unsafe extern "C-unwind" fn ctx_open(L: *mut lua_State) -> c_int {
    unsafe {
        let engine = engine();
        new_table(L);
        (*engine).var_getters_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        new_table(L);
        step1::bind(L, var_index, 0);
        require(L, b"apisix.native.runtime");
        lua_getfield(L, -1, cstr!("cached_index"));
        lua_remove(L, -2);
        lua_insert(L, -2);
        lua_call(L, 1, 1);
        lua_setfield(L, -2, cstr!("__index"));
        push_fn(L, var_newindex);
        lua_setfield(L, -2, cstr!("__newindex"));
        (*engine).var_meta_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        new_table(L);
        register(
            L,
            &[(b"set_vars_meta\0", set_vars_meta), (b"release_vars\0", release_vars), (b"register_var\0", register_var)],
        );
        1
    }
}

// --------------------------------------------- apisix.consumer / utils.auth

/// `ac_consumer_plugin`: the consumers of one auth plugin.
unsafe extern "C-unwind" fn consumer_plugin(L: *mut lua_State) -> c_int {
    unsafe {
        push_ref(L, step1::current_lua(L).consumer_plugins_ref);
        lua_pushvalue(L, 1);
        lua_rawget(L, -2);
        1
    }
}

/// `ac_consumer_kv`: key attribute -> consumer, cached per plugin group.
unsafe extern "C-unwind" fn consumer_kv(L: *mut lua_State) -> c_int {
    unsafe {
        let attribute = check_bytes(L, 3);
        push_ref(L, step1::current_lua(L).consumer_indexes_ref);
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        if is_nil(L, -1) {
            pop(L, 1);
            new_table(L);
            lua_pushvalue(L, 2);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
        }
        lua_remove(L, -2);
        let cache = lua_gettop(L);
        push_bytes(L, attribute);
        lua_rawget(L, cache);
        if !is_nil(L, -1) {
            return 1;
        }
        pop(L, 1);
        new_table(L);
        let values = lua_gettop(L);
        lua_getfield(L, 2, cstr!("nodes"));
        let nodes = lua_gettop(L);
        for index in 1..=lua_objlen(L, nodes) as c_int {
            lua_rawgeti(L, nodes, index);
            let consumer = lua_gettop(L);
            lua_getfield(L, consumer, cstr!("auth_conf"));
            push_bytes(L, attribute);
            lua_gettable(L, -2);
            let key = lua_gettop(L);
            if !is_nil(L, key) {
                let secret = string_bytes(L, key);
                let literal = secret.is_none_or(|value| {
                    !value.starts_with(b"$secret://") && !(value.len() >= 7 && value[..7].eq_ignore_ascii_case(b"$ENV://"))
                });
                if literal {
                    lua_pushvalue(L, key);
                    lua_pushvalue(L, consumer);
                    lua_rawset(L, values);
                }
            }
            pop(L, 3);
        }
        push_bytes(L, attribute);
        lua_pushvalue(L, values);
        lua_rawset(L, cache);
        lua_pushvalue(L, values);
        1
    }
}

unsafe extern "C-unwind" fn consumer_find(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 3);
        push_fn(L, consumer_plugin);
        lua_pushvalue(L, 1);
        lua_call(L, 1, 1);
        let conf = lua_gettop(L);
        if is_nil(L, conf) {
            lua_pushnil(L);
            lua_pushnil(L);
            push_bytes(L, b"Missing related consumer");
            return 3;
        }
        push_fn(L, consumer_kv);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, conf);
        lua_pushvalue(L, 2);
        lua_call(L, 3, 1);
        let values = lua_gettop(L);
        lua_pushvalue(L, 3);
        lua_rawget(L, values);
        lua_pushvalue(L, conf);
        2
    }
}

unsafe extern "C-unwind" fn consumer_attach(L: *mut lua_State) -> c_int {
    const FIELDS: [(&[u8], &[u8]); 2] = [(b"consumer_name\0", b"consumer_name\0"), (b"consumer_group_id\0", b"group_id\0")];
    const HEADERS: [(&[u8], &[u8]); 3] = [
        (b"X-Consumer-Username", b"username\0"),
        (b"X-Credential-Identifier", b"credential_id\0"),
        (b"X-Consumer-Custom-ID", b"custom_id\0"),
    ];
    unsafe {
        lua_settop(L, 3);
        lua_pushvalue(L, 2);
        lua_setfield(L, 1, cstr!("consumer"));
        for (field, source) in FIELDS {
            lua_getfield(L, 2, source.as_ptr() as *const c_char);
            lua_setfield(L, 1, field.as_ptr() as *const c_char);
        }
        lua_getfield(L, 3, cstr!("conf_version"));
        lua_setfield(L, 1, cstr!("consumer_ver"));
        for (header, field) in HEADERS {
            require(L, b"apisix.core.request");
            lua_getfield(L, -1, cstr!("set_header"));
            lua_pushvalue(L, 1);
            push_bytes(L, header);
            lua_getfield(L, 2, field.as_ptr() as *const c_char);
            lua_call(L, 3, 0);
            pop(L, 1);
        }
        0
    }
}

unsafe extern "C-unwind" fn consumer_anonymous(L: *mut lua_State) -> c_int {
    unsafe {
        step1::resource_get(L, b"consumers\0", 1);
        let wrapper = lua_gettop(L);
        if is_nil(L, wrapper) {
            lua_pushnil(L);
            lua_pushnil(L);
            push_bytes(L, b"failed to get anonymous consumer ");
            lua_pushvalue(L, 1);
            lua_concat(L, 2);
            return 3;
        }
        lua_getfield(L, wrapper, cstr!("value"));
        let consumer = lua_gettop(L);
        lua_getfield(L, consumer, cstr!("id"));
        lua_setfield(L, consumer, cstr!("consumer_name"));
        lua_getfield(L, wrapper, cstr!("modifiedIndex"));
        lua_setfield(L, consumer, cstr!("modifiedIndex"));
        new_table(L);
        lua_getfield(L, wrapper, cstr!("modifiedIndex"));
        lua_setfield(L, -2, cstr!("conf_version"));
        2
    }
}

unsafe extern "C-unwind" fn consumers(L: *mut lua_State) -> c_int {
    unsafe {
        step1::resource(L, b"consumers\0");
        lua_getfield(L, -1, cstr!("values"));
        lua_getfield(L, -2, cstr!("conf_version"));
        2
    }
}

unsafe extern "C-unwind" fn consumer_open(L: *mut lua_State) -> c_int {
    unsafe {
        new_table(L);
        register(
            L,
            &[
                (b"plugin\0", consumer_plugin),
                (b"consumers_conf\0", consumer_plugin),
                (b"consumers_kv\0", consumer_kv),
                (b"find_consumer\0", consumer_find),
                (b"attach_consumer\0", consumer_attach),
                (b"get_anonymous_consumer\0", consumer_anonymous),
                (b"consumers\0", consumers),
            ],
        );
        1
    }
}

unsafe extern "C-unwind" fn multi_auth(L: *mut lua_State) -> c_int {
    unsafe {
        lua_getfield(L, 1, cstr!("_plugin_name"));
        let running = string_bytes(L, -1) == Some(b"multi-auth".as_slice());
        lua_pushboolean(L, running as c_int);
        1
    }
}

unsafe extern "C-unwind" fn auth_open(L: *mut lua_State) -> c_int {
    unsafe {
        new_table(L);
        push_fn(L, multi_auth);
        lua_setfield(L, -2, cstr!("is_running_under_multi_auth"));
        1
    }
}

/// `ac_trace_release` with tracing disabled (rejected at compile time):
/// nothing was pooled for this request.
unsafe extern "C-unwind" fn tracer_release(L: *mut lua_State) -> c_int {
    unsafe {
        lua_getfield(L, 1, cstr!("tracing"));
        if is_table(L, -1) {
            fail(L, b"tracing is not provided by the Rust step-1 core");
        }
        0
    }
}

unsafe extern "C-unwind" fn tracer_open(L: *mut lua_State) -> c_int {
    unsafe {
        new_table(L);
        push_fn(L, tracer_release);
        lua_setfield(L, -2, cstr!("release"));
        1
    }
}

/// Keeps `c_void` imported for the lightuserdata helpers in this module.
#[allow(dead_code)]
fn _unused(_: *mut c_void) {}
