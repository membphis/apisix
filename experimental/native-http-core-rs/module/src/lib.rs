// SPDX-License-Identifier: Apache-2.0
//! `ngx_http_apisix_rs_module`: Rust port of the APISIX v2 native key-auth
//! request pipeline (`experimental/native-http-core`, `apisix_core_native_pipeline`).
//!
//! Two modes share the core, the generation runtime and the variables:
//!
//! v2 pipeline (mirroring the C module's pipeline mode, no Lua per request):
//! - `apisix_rs_config <path>;`  (http)     standalone `native.json`, watched every second
//! - `apisix_rs_proxy on|off;`   (location) run the native access/log pipeline
//! - `apisix_rs_balancer;`       (upstream) native peer selection; must precede `keepalive`
//!
//! Step 1, Rust core + unchanged Lua plugins (mirroring C native-lua, V1):
//! - `apisix_rs_lua_plugins on;` (http)     provide `apisix.native.engine` and the SDK
//!   modules to Lua; `runtime.lua` drives the phases (`step1.rs`, `sdk.rs`)
//! - `apisix_rs_proxy on;`       (location) the proxied business location
//!
//! NGINX-independent logic lives in the `apisix-rs-core` crate; this crate
//! holds all FFI. Every NGINX entry point contains panics (`guard::contain`),
//! so a Rust panic fails one request with 500 instead of aborting the worker;
//! Lua-facing step-1 functions cannot contain panics this way (`lua.rs`).
#![deny(clippy::unwrap_used, clippy::expect_used)]
#![allow(non_upper_case_globals)]

mod filters;
mod guard;
mod http;
mod keyauth;
mod lua;
mod peer;
mod pipeline;
mod runtime;
mod sdk;
mod step1;
mod vars;

use core::ffi::{c_char, c_void};
use core::ptr;

use ngx::ffi::*;
use ngx::http::HttpModuleConfExt;
use ngx::ngx_string;

#[repr(C)]
pub(crate) struct MainConf {
    pub(crate) config_path: ngx_str_t,
    pub(crate) lua_plugins: ngx_flag_t,
}

#[repr(C)]
pub(crate) struct LocConf {
    pub(crate) proxy: ngx_flag_t,
}

static mut COMMANDS: [ngx_command_t; 5] = [
    ngx_command_t {
        name: ngx_string!("apisix_rs_config"),
        type_: (NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1) as ngx_uint_t,
        set: Some(ngx_conf_set_str_slot),
        conf: NGX_HTTP_MAIN_CONF_OFFSET,
        offset: core::mem::offset_of!(MainConf, config_path),
        post: ptr::null_mut(),
    },
    ngx_command_t {
        name: ngx_string!("apisix_rs_proxy"),
        type_: (NGX_HTTP_LOC_CONF | NGX_CONF_FLAG) as ngx_uint_t,
        set: Some(ngx_conf_set_flag_slot),
        conf: NGX_HTTP_LOC_CONF_OFFSET,
        offset: core::mem::offset_of!(LocConf, proxy),
        post: ptr::null_mut(),
    },
    ngx_command_t {
        name: ngx_string!("apisix_rs_lua_plugins"),
        type_: (NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG) as ngx_uint_t,
        set: Some(ngx_conf_set_flag_slot),
        conf: NGX_HTTP_MAIN_CONF_OFFSET,
        offset: core::mem::offset_of!(MainConf, lua_plugins),
        post: ptr::null_mut(),
    },
    ngx_command_t {
        name: ngx_string!("apisix_rs_balancer"),
        type_: (NGX_HTTP_UPS_CONF | NGX_CONF_NOARGS) as ngx_uint_t,
        set: Some(peer::balancer_directive),
        conf: 0,
        offset: 0,
        post: ptr::null_mut(),
    },
    ngx_command_t::empty(),
];

static MODULE_CTX: ngx_http_module_t = ngx_http_module_t {
    preconfiguration: Some(preconfiguration),
    postconfiguration: Some(postconfiguration),
    create_main_conf: Some(create_main_conf),
    init_main_conf: None,
    create_srv_conf: None,
    merge_srv_conf: None,
    create_loc_conf: Some(create_loc_conf),
    merge_loc_conf: Some(merge_loc_conf),
};

#[no_mangle]
pub static mut ngx_http_apisix_rs_module: ngx_module_t = ngx_module_t {
    ctx: ptr::addr_of!(MODULE_CTX) as *mut c_void,
    commands: ptr::addr_of_mut!(COMMANDS) as *mut ngx_command_t,
    type_: NGX_HTTP_MODULE as ngx_uint_t,
    init_process: Some(runtime::init_process),
    exit_process: Some(runtime::exit_process),
    ..ngx_module_t::default()
};

ngx::ngx_modules!(ngx_http_apisix_rs_module);

const _: () = {
    // The pure crate carries NGINX method bits; keep them in sync with the bindings.
    let methods = apisix_rs_core::config::METHODS;
    assert!(methods[0].1 == NGX_HTTP_GET && methods[1].1 == NGX_HTTP_HEAD && methods[2].1 == NGX_HTTP_POST);
    assert!(methods[3].1 == NGX_HTTP_PUT && methods[4].1 == NGX_HTTP_DELETE && methods[5].1 == NGX_HTTP_OPTIONS);
    assert!(methods[6].1 == NGX_HTTP_PATCH && methods[7].1 == NGX_HTTP_TRACE && methods[8].1 == NGX_HTTP_CONNECT);
};

/// This module's descriptor (read-only after NGINX assigned its indexes).
#[inline]
pub(crate) fn module() -> &'static ngx_module_t {
    unsafe { &*ptr::addr_of!(ngx_http_apisix_rs_module) }
}

unsafe extern "C" fn create_main_conf(cf: *mut ngx_conf_t) -> *mut c_void {
    // Zeroed: an unset path has a null pointer, as ngx_conf_set_str_slot expects.
    let conf = unsafe { ngx_pcalloc((*cf).pool, core::mem::size_of::<MainConf>()) } as *mut MainConf;
    if !conf.is_null() {
        unsafe { (*conf).lua_plugins = NGX_CONF_UNSET as ngx_flag_t };
    }
    conf as *mut c_void
}

unsafe extern "C" fn create_loc_conf(cf: *mut ngx_conf_t) -> *mut c_void {
    let conf = unsafe { ngx_pcalloc((*cf).pool, core::mem::size_of::<LocConf>()) } as *mut LocConf;
    if !conf.is_null() {
        unsafe { (*conf).proxy = NGX_CONF_UNSET as ngx_flag_t };
    }
    conf as *mut c_void
}

unsafe extern "C" fn merge_loc_conf(_cf: *mut ngx_conf_t, parent: *mut c_void, child: *mut c_void) -> *mut c_char {
    let (prev, conf) = (parent as *mut LocConf, child as *mut LocConf);
    unsafe {
        if (*conf).proxy == NGX_CONF_UNSET as ngx_flag_t {
            (*conf).proxy = if (*prev).proxy == NGX_CONF_UNSET as ngx_flag_t { 0 } else { (*prev).proxy };
        }
    }
    ptr::null_mut()
}

unsafe extern "C" fn preconfiguration(cf: *mut ngx_conf_t) -> ngx_int_t {
    guard::install_panic_hook();
    let log = unsafe { (*cf).log };
    guard::contain(log, NGX_ERROR as ngx_int_t, "preconfiguration", || unsafe { vars::register(cf) })
}

unsafe extern "C" fn postconfiguration(cf: *mut ngx_conf_t) -> ngx_int_t {
    let log = unsafe { (*cf).log };
    guard::contain(log, NGX_ERROR as ngx_int_t, "postconfiguration", || unsafe {
        let Some(main) = (*cf).http_main_conf_unchecked::<MainConf>(module()) else {
            return NGX_ERROR as ngx_int_t;
        };
        let main = main.as_ptr();
        let path = http::bytes(&(*main).config_path);
        if (*main).lua_plugins == 1 {
            // Step 1: configuration arrives through engine.init in init_by_lua.
            if !path.is_empty() {
                ngx::ngx_conf_log_error!(NGX_LOG_EMERG, cf, "apisix_rs_lua_plugins and apisix_rs_config are exclusive");
                return NGX_ERROR as ngx_int_t;
            }
            for (name, open) in sdk::MODULES {
                if lua::ngx_http_lua_add_package_preload(cf, name.as_ptr() as *const c_char, *open) != NGX_OK as ngx_int_t {
                    return NGX_ERROR as ngx_int_t;
                }
            }
            filters::register();
            return NGX_OK as ngx_int_t;
        }
        let Some(core) = (*cf).http_main_conf_unchecked::<ngx_http_core_main_conf_t>(&*ptr::addr_of!(ngx_http_core_module))
        else {
            return NGX_ERROR as ngx_int_t;
        };
        let core = core.as_ptr();
        let handlers: [(ngx_http_phases, ngx_http_handler_pt); 2] = [
            (ngx_http_phases_NGX_HTTP_ACCESS_PHASE, Some(pipeline::access_handler)),
            (ngx_http_phases_NGX_HTTP_LOG_PHASE, Some(pipeline::log_handler)),
        ];
        for (phase, handler) in handlers {
            let slot = ngx_array_push(&mut (*core).phases[phase as usize].handlers) as *mut ngx_http_handler_pt;
            if slot.is_null() {
                return NGX_ERROR as ngx_int_t;
            }
            *slot = handler;
        }
        // Validate the standalone configuration while loading NGINX, as the C
        // module does in init_by_lua: an invalid file fails `nginx -t`/start.
        if !path.is_empty() {
            if let Err(error) = runtime::validate(path, log) {
                ngx::ngx_conf_log_error!(NGX_LOG_EMERG, cf, "standalone configuration rejected: {}", error);
                return NGX_ERROR as ngx_int_t;
            }
            // Same native header filter as the C pipeline (native filters on).
            filters::register();
        }
        NGX_OK as ngx_int_t
    })
}
