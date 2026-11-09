// SPDX-License-Identifier: Apache-2.0
//! Step 1 of the migration: Rust core + unchanged Lua plugins.
//!
//! This ports the C prototype's V1 "native-lua" design (`module/plugins.c`,
//! `module.c`, `upstream.c`, `http.c`, `routes.c`, `snapshot.c` and the
//! unchanged `runtime.lua`) to Rust:
//!
//! - the same `apisix.native.engine` Lua module and coroutine transport
//!   (`start_phase`/`step`/`render`/`balance_start`/`balance_finish`): the
//!   core owns phase scheduling, routing, consumer-route merging, upstream
//!   preparation and peer selection; Lua only invokes plugin callbacks;
//! - the same three Lua entries (`access_by_lua`, `balancer_by_lua`,
//!   `log_by_lua`) and native header/body filters;
//! - the SDK subset key-auth needs, in `sdk.rs`.
//!
//! Routing, upstream selection and the closed-world configuration contract
//! reuse `apisix-rs-core` and the Rust v2 generation runtime. The Lua side of
//! a generation (resource wrappers, plugin plans with `check_schema`
//! defaults, consumer tables) is built in the worker's Lua VM.

#![allow(non_snake_case)] // `L` is the Lua C API's name for the state.

use core::cell::{RefCell, UnsafeCell};
use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use std::rc::Rc;

use apisix_rs_core::balancer::Peer;
use apisix_rs_core::router::host_match;
use apisix_rs_core::uri::only_decoded_or_merged;
use ngx::core::Pool;
use ngx::ffi::*;
use ngx::http::HttpModuleConfExt;
use serde_json::Value;

use crate::lua::*;
use crate::pipeline::{RequestCtx, State};
use crate::runtime::{self, GenRt};
use crate::{http, module, sdk, LocConf};

pub(crate) const REWRITE: u8 = 0;
pub(crate) const ACCESS: u8 = 1;
pub(crate) const BEFORE_PROXY: u8 = 2;
pub(crate) const HEADER_FILTER: u8 = 3;
pub(crate) const BODY_FILTER: u8 = 4;
pub(crate) const LOG: u8 = 6;
pub(crate) const PHASES: usize = 7;
const PHASE_NAMES: [&[u8]; PHASES] =
    [b"rewrite", b"access", b"before_proxy", b"header_filter", b"body_filter", b"delayed_body_filter", b"log"];
const PHASE_FIELDS: [&[u8]; PHASES] = [
    b"rewrite\0",
    b"access\0",
    b"before_proxy\0",
    b"header_filter\0",
    b"body_filter\0",
    b"delayed_body_filter\0",
    b"log\0",
];
const KEEPALIVE_SIZE: u32 = 320;
const KEEPALIVE_REQUESTS: u32 = 1000;
const KEEPALIVE_IDLE_MS: u64 = 60_000;
const MERGE_CACHE: usize = 256;
/// Large enough for any `ngx_sock_ntop` text form (`NGX_SOCKADDR_STRLEN`).
const SOCKADDR_STRLEN: usize = 128;
const TABLE_POOL: usize = 200;

// ------------------------------------------------------------ worker state

pub(crate) struct Plugin {
    pub(crate) name: Vec<u8>,
    pub(crate) module_ref: c_int,
    pub(crate) priority: f64,
    pub(crate) auth: bool,
    pub(crate) callbacks: [c_int; PHASES],
}

impl Plugin {
    #[inline]
    fn has(&self, phase: u8) -> bool {
        let callback = self.callbacks[phase as usize];
        callback != LUA_REFNIL && callback != LUA_NOREF
    }
}

pub(crate) struct Engine {
    pub(crate) vm: *mut lua_State,
    initialized: bool,
    compiled: bool,
    exiting: bool,
    path: String,
    source: Vec<u8>,
    plugins: Vec<Plugin>,
    pub(crate) var_meta_ref: c_int,
    pub(crate) var_getters_ref: c_int,
    /// The newest generation's Lua data, as the C core reads `ac_main()`.
    current: *const LuaGen,
}

/// `request_pool_native`: recycled api_ctx tables (`ac_table_pool_t`).
struct TablePool {
    refs: Vec<c_int>,
    releases: u32,
}

thread_local! {
    static ENGINE: UnsafeCell<Engine> = const {
        UnsafeCell::new(Engine {
            vm: ptr::null_mut(),
            initialized: false,
            compiled: false,
            exiting: false,
            path: String::new(),
            source: Vec::new(),
            plugins: Vec::new(),
            var_meta_ref: LUA_NOREF,
            var_getters_ref: LUA_NOREF,
            current: ptr::null(),
        })
    };
    static TABLES: UnsafeCell<TablePool> = const { UnsafeCell::new(TablePool { refs: Vec::new(), releases: 0 }) };
}

#[inline]
pub(crate) fn engine() -> *mut Engine {
    ENGINE.with(UnsafeCell::get)
}

/// The loaded plugins (written once, by `init_worker`).
#[inline]
fn loaded<'a>() -> &'a [Plugin] {
    unsafe { &(*engine()).plugins }
}

#[inline]
fn tables() -> *mut TablePool {
    TABLES.with(UnsafeCell::get)
}

/// The VM for releasing registry references, unless the worker is exiting
/// (the C core skips Lua cleanup then, too).
fn release_vm() -> Option<*mut lua_State> {
    let engine = engine();
    unsafe {
        if (*engine).vm.is_null() || (*engine).exiting || ngx_exiting != 0 || ngx_terminate != 0 || ngx_quit != 0 {
            None
        } else {
            Some((*engine).vm)
        }
    }
}

// --------------------------------------------------- Lua side of a generation

pub(crate) struct Call {
    plugin: usize,
    conf_ref: c_int,
    priority: f64,
    consumer_only: bool,
}

/// `ac_metadata_t`: conf_type, conf_id, conf_version, route_name.
struct Meta([c_int; 4]);

const META_FIELDS: [&[u8]; 4] = [b"conf_type\0", b"conf_id\0", b"conf_version\0", b"route_name\0"];

struct LuaRoute {
    wrapper_ref: c_int,
    value_ref: c_int,
    upstream_conf_ref: c_int,
    calls: Vec<Call>,
    meta: Meta,
}

/// One cached consumer-route combination (`ac_consumer_route_t`).
pub(crate) struct Merged {
    route: usize,
    consumer: *const c_void,
    changed: bool,
    wrapper_ref: c_int,
    calls: Option<Vec<Call>>,
    meta: Meta,
    refs: Vec<c_int>,
}

pub(crate) struct LuaGen {
    store_ref: c_int,
    routes: Vec<LuaRoute>,
    pub(crate) consumer_plugins_ref: c_int,
    pub(crate) consumer_indexes_ref: c_int,
    /// Boxed: requests keep raw pointers to entries while the vector grows.
    #[allow(clippy::vec_box)]
    merged: RefCell<Vec<Box<Merged>>>,
    refs: Vec<c_int>,
}

impl Drop for LuaGen {
    fn drop(&mut self) {
        let engine = engine();
        unsafe {
            if ptr::eq((*engine).current, self) {
                (*engine).current = ptr::null();
            }
        }
        let Some(L) = release_vm() else { return };
        for reference in self.refs.drain(..) {
            unsafe { unref(L, reference) };
        }
        for merged in self.merged.get_mut().drain(..) {
            for reference in &merged.refs {
                unsafe { unref(L, *reference) };
            }
        }
    }
}

/// The current generation's Lua data (SDK consumer lookups).
pub(crate) unsafe fn current_lua<'a>(L: *mut lua_State) -> &'a LuaGen {
    let current = unsafe { (*engine()).current };
    if current.is_null() {
        unsafe { fail(L, b"native core has not been initialized") }
    }
    unsafe { &*current }
}

/// `ac_resource`: pushes the store object of `kind` (NUL-terminated).
pub(crate) unsafe fn resource(L: *mut lua_State, kind: &[u8]) {
    unsafe {
        push_ref(L, current_lua(L).store_ref);
        lua_getfield(L, -1, kind.as_ptr() as *const c_char);
        lua_remove(L, -2);
    }
}

/// `ac_resource_get`: pushes the wrapper with id = value at `key`, or nil.
pub(crate) unsafe fn resource_get(L: *mut lua_State, kind: &[u8], key: c_int) {
    unsafe {
        let key = absolute(L, key);
        resource(L, kind);
        let object = lua_gettop(L);
        if is_nil(L, object) {
            return;
        }
        lua_getfield(L, object, cstr!("values_hash"));
        lua_pushvalue(L, key);
        lua_tolstring(L, -1, ptr::null_mut());
        lua_rawget(L, -2);
        if is_nil(L, -1) {
            lua_replace(L, object);
            lua_settop(L, object);
            return;
        }
        lua_getfield(L, object, cstr!("values"));
        lua_rawgeti(L, -1, lua_tointeger(L, -2) as c_int);
        lua_replace(L, object);
        lua_settop(L, object);
    }
}

// ------------------------------------------------------- per-request state

pub(crate) struct LuaRequest {
    table_ref: c_int,
    phase: u8,
    stage: u32,
    cursor: usize,
    active: usize,
    callback_pending: bool,
    phase_entered: bool,
    early_status: u16,
    consumer_processed: bool,
    consumer_rewrite: bool,
    /// Consumer-route combination in use, owned by the pinned generation
    /// (or by `owned` beyond the cache bound).
    merged: *const Merged,
    owned: Option<Box<Merged>>,
    upstream_prepared: bool,
    upstream_logged: bool,
    log_done: bool,
    normalized: ngx_str_t,
}

impl LuaRequest {
    fn new() -> LuaRequest {
        LuaRequest {
            table_ref: LUA_NOREF,
            phase: PHASES as u8,
            stage: 0,
            cursor: 0,
            active: 0,
            callback_pending: false,
            phase_entered: false,
            early_status: 0,
            consumer_processed: false,
            consumer_rewrite: false,
            merged: ptr::null(),
            owned: None,
            upstream_prepared: false,
            upstream_logged: false,
            log_done: false,
            normalized: ngx_str_t { len: 0, data: ptr::null_mut() },
        }
    }
}

#[inline]
unsafe fn lua_of<'a>(ctx: *mut RequestCtx) -> &'a mut LuaRequest {
    // Contexts handed to Lua-engine code always carry the Lua state.
    match unsafe { (*ctx).lua.as_mut() } {
        Some(lua) => lua,
        None => unreachable_lua(),
    }
}

#[cold]
fn unreachable_lua() -> ! {
    panic!("request context without Lua engine state")
}

#[inline]
pub(crate) unsafe fn table_ref(ctx: *mut RequestCtx) -> c_int {
    unsafe { lua_of(ctx).table_ref }
}

#[inline]
unsafe fn generation<'a>(ctx: *mut RequestCtx) -> &'a GenRt {
    // The Rc lives in the request context, which outlives every engine call.
    unsafe { &*Rc::as_ptr(&(*ctx).generation) }
}

#[inline]
unsafe fn lua_gen(generation: &GenRt) -> &LuaGen {
    match generation.lua.as_deref() {
        Some(lua) => lua,
        None => unreachable_lua(),
    }
}

unsafe fn request(L: *mut lua_State) -> *mut ngx_http_request_t {
    let r = unsafe { ngx_http_lua_get_request(L) };
    if r.is_null() {
        unsafe { fail(L, b"native core API requires an HTTP request") }
    }
    r
}

#[inline]
unsafe fn slot(r: *mut ngx_http_request_t) -> *mut *mut RequestCtx {
    unsafe { (*r).ctx.add(module().ctx_index) as *mut *mut RequestCtx }
}

/// `ac_request`: the request's engine context, created on first use.
pub(crate) unsafe fn context(L: *mut lua_State) -> *mut RequestCtx {
    unsafe {
        let r = request(L);
        let ctx = *slot(r);
        if !ctx.is_null() {
            if (*ctx).lua.is_none() {
                fail(L, b"C-only request cannot enter the Lua engine");
            }
            return ctx;
        }
        let Some(generation) = runtime::current() else { fail(L, b"native core has not been initialized") };
        let pool = Pool::from_ngx_pool((*r).pool);
        let ctx = pool.allocate(RequestCtx { generation, state: State::new(), lua: Some(LuaRequest::new()) });
        if ctx.is_null() {
            fail(L, b"native core request allocation failed");
        }
        *slot(r) = ctx;
        attach_lua_context(L, ctx);
        ctx
    }
}

/// `ac_table_pool_fetch` for the api_ctx table.
unsafe fn table_fetch(L: *mut lua_State) -> c_int {
    unsafe {
        if let Some(reference) = (*tables()).refs.pop() {
            return reference;
        }
        lua_createtable(L, 0, 32);
        luaL_ref(L, LUA_REGISTRYINDEX)
    }
}

/// `ac_attach_lua_context`: `ngx.ctx.api_ctx` and the `ctx.var` view.
unsafe fn attach_lua_context(L: *mut lua_State, ctx: *mut RequestCtx) {
    unsafe {
        let table = table_fetch(L);
        lua_of(ctx).table_ref = table;
        get_global(L, cstr!("ngx"));
        lua_getfield(L, -1, cstr!("ctx"));
        push_ref(L, table);
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, cstr!("ngx_ctx"));
        lua_setfield(L, -2, cstr!("api_ctx"));
        pop(L, 2);
        require(L, b"apisix.core.ctx");
        lua_getfield(L, -1, cstr!("set_vars_meta"));
        push_ref(L, table);
        lua_call(L, 1, 0);
        pop(L, 1);
    }
}

/// Pool cleanup of a Lua-engine request (`ac_cleanup`).
pub(crate) fn cleanup(ctx: &mut RequestCtx) {
    let Some(mut lua) = ctx.lua.take() else { return };
    let Some(L) = release_vm() else { return };
    let generation: &GenRt = &ctx.generation;
    if let (Some(route), Some(peer)) = (ctx.state.route, ctx.state.peer.as_mut()) {
        if let Some(upstream) = generation.core.routes[route].upstream.as_ref() {
            upstream.release(peer, false, unsafe { ngx_current_msec } as u64);
        }
    }
    unsafe {
        if let Some(owned) = lua.owned.take() {
            for reference in &owned.refs {
                unref(L, *reference);
            }
        }
        if lua.log_done && table_return(L, lua.table_ref) {
            lua.table_ref = LUA_NOREF;
        }
        unref(L, lua.table_ref);
    }
}

unsafe extern "C-unwind" fn table_clear_call(L: *mut lua_State) -> c_int {
    unsafe {
        lua_getfield(L, LUA_REGISTRYINDEX, cstr!("_LOADED"));
        lua_getfield(L, -1, cstr!("table.clear"));
        if !is_function(L, -1) {
            lua_pushboolean(L, 0);
            return 1;
        }
        lua_pushvalue(L, 1);
        lua_call(L, 1, 0);
        lua_pushboolean(L, 1);
        1
    }
}

/// `ac_table_pool_return`: clears and keeps an api_ctx table; full pools and
/// every 20000th release fall back to the GC, as the installed tablepool.
unsafe fn table_return(L: *mut lua_State, reference: c_int) -> bool {
    unsafe {
        let pool = tables();
        if reference < 0 {
            return false;
        }
        let top = lua_gettop(L);
        push_ref(L, reference);
        if lua_getmetatable(L, -1) != 0 {
            lua_settop(L, top);
            return false;
        }
        (*pool).releases += 1;
        if (*pool).releases >= 20000 {
            for kept in (*pool).refs.drain(..) {
                unref(L, kept);
            }
            (*pool).releases = 0;
            lua_settop(L, top);
            return false;
        }
        if (*pool).refs.len() == TABLE_POOL {
            lua_settop(L, top);
            return false;
        }
        push_fn(L, table_clear_call);
        lua_insert(L, -2);
        let retained = if lua_pcall(L, 1, 1, 0) != 0 {
            let message = to_bytes(L, -1).map(|text| String::from_utf8_lossy(text).into_owned()).unwrap_or_default();
            ngx::ngx_log_error!(NGX_LOG_ERR, (*ngx_cycle).log, "native registry api_ctx pool clear failed: {}", message);
            false
        } else {
            truthy(L, -1)
        };
        lua_settop(L, top);
        if retained {
            (*pool).refs.push(reference);
        }
        retained
    }
}

// ----------------------------------------------------------- engine module

pub(crate) unsafe extern "C-unwind" fn engine_open(L: *mut lua_State) -> c_int {
    let functions: [(&[u8], lua_CFunction); 9] = [
        (b"init\0", init),
        (b"init_worker\0", init_worker),
        (b"shutdown\0", shutdown),
        (b"context\0", context_api),
        (b"start_phase\0", START_PHASE),
        (b"step\0", step),
        (b"render\0", render),
        (b"balance_start\0", balance_start),
        (b"balance_finish\0", balance_finish),
    ];
    unsafe {
        new_table(L);
        for (name, function) in functions {
            push_fn(L, function);
            lua_setfield(L, -2, name.as_ptr() as *const c_char);
        }
    }
    1
}

/// `ac_bind`: wraps a native start function in `runtime.bind`.
pub(crate) unsafe fn bind(L: *mut lua_State, start: lua_CFunction, upvalues: c_int) {
    unsafe {
        lua_pushcclosure(L, start, upvalues);
        require(L, b"apisix.native.runtime");
        lua_getfield(L, -1, cstr!("bind"));
        lua_remove(L, -2);
        lua_insert(L, -2);
        lua_call(L, 1, 1);
    }
}

unsafe extern "C-unwind" fn context_api(L: *mut lua_State) -> c_int {
    unsafe {
        let ctx = context(L);
        push_ref(L, lua_of(ctx).table_ref);
    }
    1
}

#[inline]
unsafe fn upstream_enabled(r: *mut ngx_http_request_t) -> bool {
    unsafe { (*r).http_location_conf_unchecked::<LocConf>(module()) }.is_some_and(|conf| unsafe { (*conf.as_ptr()).proxy } == 1)
}

/// `ac_enter`.
unsafe fn enter(L: *mut lua_State) -> bool {
    unsafe {
        let name = check_bytes(L, 1);
        let r = request(L);
        if (*slot(r)).is_null() && name != b"access" {
            return false;
        }
        let ctx = context(L);
        let lua = lua_of(ctx);
        if lua.log_done {
            return false;
        }
        let Some(index) = PHASE_NAMES.iter().position(|phase| *phase == name) else {
            let mut message = b"unknown native HTTP phase '".to_vec();
            message.extend_from_slice(name);
            message.push(b'\'');
            fail(L, &message)
        };
        let index = index as u8;
        lua.phase = if index == ACCESS { REWRITE } else { index };
        lua.stage = 0;
        lua.cursor = 0;
        lua.callback_pending = false;
        lua.phase_entered = true;
        // Tracing is rejected at compile time; key-auth has no log callback,
        // so the C core's log-variable prefetch has nothing to publish.
        if index == ACCESS {
            let status = http_prepare(L, ctx, r);
            lua_of(ctx).early_status = status;
            if status == 0 {
                match_route(L, ctx, r);
            }
        } else if index == HEADER_FILTER || index == BODY_FILTER {
            // Native filters: startup rejects Lua filter callbacks.
            lua_of(ctx).phase_entered = false;
            return false;
        }
        true
    }
}

unsafe extern "C-unwind" fn start_phase(L: *mut lua_State) -> c_int {
    unsafe {
        if !enter(L) {
            lua_settop(L, 0);
            lua_pushnil(L);
            return 1;
        }
        phase_next(L)
    }
}

/// `ac_phase_next`: the next callable, or nil plus `ac_leave` results.
unsafe fn phase_next(L: *mut lua_State) -> c_int {
    unsafe {
        lua_settop(L, 0);
        let count = next(L);
        if is_function(L, -count) {
            return count;
        }
        lua_settop(L, 0);
        let count = leave(L);
        lua_pushnil(L);
        if count > 0 {
            lua_insert(L, -(count + 1));
        }
        count + 1
    }
}

unsafe extern "C-unwind" fn step(L: *mut lua_State) -> c_int {
    unsafe {
        let count = complete(L);
        if count > 0 {
            lua_pushnil(L);
            lua_insert(L, -(count + 1));
            return count + 1;
        }
        phase_next(L)
    }
}

/// The plan of the current stage: odd stages run the (possibly
/// consumer-merged) route plan; even stages run the global plan, which is
/// empty because global rules are rejected at compile time.
unsafe fn current_calls<'a>(generation: &'a GenRt, lua: &LuaRequest, route: Option<usize>) -> &'a [Call] {
    unsafe {
        if lua.stage.is_multiple_of(2) {
            return &[];
        }
        if !lua.merged.is_null() {
            if let Some(calls) = (*lua.merged).calls.as_ref() {
                return calls;
            }
        }
        match route {
            Some(route) => &lua_gen(generation).routes[route].calls,
            None => &[],
        }
    }
}

/// `ac_enabled` without filter expressions (rejected at compile time).
unsafe fn enabled(L: *mut lua_State, table: c_int, plugin: &Plugin) -> bool {
    unsafe {
        push_ref(L, table);
        lua_getfield(L, -1, cstr!("_skip_plugins"));
        let mut allowed = true;
        if is_table(L, -1) {
            push_bytes(L, &plugin.name);
            lua_rawget(L, -2);
            allowed = !truthy(L, -1);
            pop(L, 1);
        }
        pop(L, 2);
        allowed
    }
}

/// `ac_next`.
unsafe fn next(L: *mut lua_State) -> c_int {
    unsafe {
        let ctx = context(L);
        let r = request(L);
        let generation = generation(ctx);
        let lua = lua_of(ctx);
        if !lua.phase_entered {
            lua_pushnil(L);
            return 1;
        }
        if lua.callback_pending {
            fail(L, b"native callback must complete before advancing");
        }
        if lua.early_status != 0 {
            lua.phase_entered = false;
            lua_pushnil(L);
            return 1;
        }
        loop {
            let lua = lua_of(ctx);
            if lua.phase == BEFORE_PROXY && !lua.upstream_prepared && upstream_enabled(r) {
                if let Some(status) = upstream_prepare(L, ctx, r) {
                    let lua = lua_of(ctx);
                    lua.early_status = status;
                    lua.phase_entered = false;
                    lua_pushnil(L);
                    return 1;
                }
                lua_of(ctx).upstream_prepared = true;
            }
            let lua = lua_of(ctx);
            let calls = current_calls(generation, lua, (*ctx).state.route);
            while lua_of(ctx).cursor < calls.len() {
                let lua = lua_of(ctx);
                let index = lua.cursor;
                lua.cursor += 1;
                let call = &calls[index];
                let plugin = &loaded()[call.plugin];
                let phase = lua.phase;
                if !plugin.has(phase)
                    || (lua.consumer_rewrite && (!call.consumer_only || plugin.auth))
                    || !enabled(L, lua.table_ref, plugin)
                {
                    continue;
                }
                return return_call(L, ctx, index, call, plugin, phase);
            }
            if !advance(L, ctx) {
                break;
            }
        }
        lua_of(ctx).phase_entered = false;
        lua_pushnil(L);
        1
    }
}

/// `ac_advance`.
unsafe fn advance(L: *mut lua_State, ctx: *mut RequestCtx) -> bool {
    unsafe {
        let routed = (*ctx).state.route.is_some();
        let lua = lua_of(ctx);
        lua.stage += 1;
        lua.cursor = 0;
        if lua.stage % 2 != 0 {
            return true;
        }
        if lua.phase == REWRITE {
            if !lua.consumer_processed && routed {
                lua.consumer_processed = true;
                if consumer_merge(L, ctx) {
                    let lua = lua_of(ctx);
                    lua.consumer_rewrite = true;
                    lua.stage = 1;
                    return true;
                }
                if lua_of(ctx).early_status != 0 {
                    return false;
                }
            }
            let lua = lua_of(ctx);
            lua.consumer_rewrite = false;
            lua.phase = ACCESS;
            return true;
        }
        if lua.phase == ACCESS && routed {
            lua.phase = BEFORE_PROXY;
            return true;
        }
        false
    }
}

/// `ac_return_call` (phase_context off, as in the measured C variant).
unsafe fn return_call(L: *mut lua_State, ctx: *mut RequestCtx, index: usize, call: &Call, plugin: &Plugin, phase: u8) -> c_int {
    unsafe {
        let lua = lua_of(ctx);
        lua.active = index;
        lua.callback_pending = true;
        let global = lua.stage % 2 == 0;
        let table = lua.table_ref;
        route_context(L, ctx, global);
        push_ref(L, table);
        push_bytes(L, &plugin.name);
        lua_setfield(L, -2, cstr!("_plugin_name"));
        pop(L, 1);
        push_ref(L, plugin.callbacks[phase as usize]);
        push_ref(L, call.conf_ref);
        push_ref(L, table);
        3
    }
}

/// `ac_complete`.
unsafe fn complete(L: *mut lua_State) -> c_int {
    unsafe {
        let ctx = context(L);
        let count = lua_gettop(L);
        let lua = lua_of(ctx);
        if !lua.callback_pending {
            fail(L, b"native callback completion without a pending callback");
        }
        lua.callback_pending = false;
        if !truthy(L, 1) {
            lua_pushvalue(L, 2);
            lua_error(L);
        }
        let table = lua.table_ref;
        push_ref(L, table);
        lua_pushnil(L);
        lua_setfield(L, -2, cstr!("_plugin_name"));
        pop(L, 1);
        route_context(L, ctx, false);
        let lua = lua_of(ctx);
        if lua.phase <= BEFORE_PROXY && ((count >= 2 && truthy(L, 2)) || (count >= 3 && truthy(L, 3))) {
            let status = luaL_checknumber(L, 2);
            if status >= 400.0 {
                let generation = generation(ctx);
                let calls = current_calls(generation, lua, (*ctx).state.route);
                if let Some(call) = calls.get(lua.active) {
                    push_ref(L, call.conf_ref);
                    lua_getfield(L, -1, cstr!("_meta"));
                    if is_table(L, -1) {
                        lua_getfield(L, -1, cstr!("error_response"));
                        if !is_nil(L, -1) {
                            lua_pushnumber(L, status);
                            lua_insert(L, -2);
                            return 2;
                        }
                        pop(L, 1);
                    }
                }
                lua_settop(L, count);
            }
            lua_pushnumber(L, status);
            if count >= 3 {
                lua_pushvalue(L, 3);
            } else {
                lua_pushnil(L);
            }
            return 2;
        }
        0
    }
}

/// `ac_leave`.
unsafe fn leave(L: *mut lua_State) -> c_int {
    unsafe {
        let ctx = context(L);
        let r = request(L);
        let lua = lua_of(ctx);
        if lua.callback_pending || lua.phase_entered {
            fail(L, b"cannot leave a native phase with pending callbacks");
        }
        let table = lua.table_ref;
        if lua.phase == LOG {
            upstream_log(ctx, r);
            require(L, b"apisix.core.ctx");
            lua_getfield(L, -1, cstr!("release_vars"));
            push_ref(L, table);
            lua_call(L, 1, 0);
            pop(L, 1);
            require(L, b"apisix.tracer");
            lua_getfield(L, -1, cstr!("release"));
            push_ref(L, table);
            lua_getfield(L, -1, cstr!("ngx_ctx"));
            lua_remove(L, -2);
            lua_call(L, 1, 0);
            pop(L, 1);
            push_ref(L, table);
            lua_pushnil(L);
            lua_setfield(L, -2, cstr!("ngx_ctx"));
            pop(L, 1);
            lua_of(ctx).log_done = true;
        }
        let lua = lua_of(ctx);
        if lua.early_status != 0 && lua.phase <= BEFORE_PROXY {
            lua_pushinteger(L, lua.early_status as isize);
            lua.early_status = 0;
            return 1;
        }
        if lua.phase == ACCESS && (*ctx).state.route.is_none() {
            lua_pushinteger(L, NGX_HTTP_NOT_FOUND as isize);
            new_table(L);
            push_bytes(L, b"404 Route Not Found");
            lua_setfield(L, -2, cstr!("error_msg"));
            return 2;
        }
        if lua.phase == BEFORE_PROXY && upstream_enabled(r) {
            push_ref(L, table);
            lua_getfield(L, -1, cstr!("_resp_source"));
            if !truthy(L, -1) {
                lua_pushboolean(L, 1);
                lua_setfield(L, -3, cstr!("_apisix_proxied"));
            }
            pop(L, 2);
        }
        0
    }
}

/// `ac_route_context` with prepared metadata.
unsafe fn route_context(L: *mut lua_State, ctx: *mut RequestCtx, global: bool) {
    unsafe {
        let table = lua_of(ctx).table_ref;
        push_ref(L, table);
        let target = lua_gettop(L);
        if global {
            push_bytes(L, b"global_rule");
            lua_setfield(L, target, cstr!("conf_type"));
            lua_pushinteger(L, 1);
            lua_setfield(L, target, cstr!("conf_version"));
            lua_pushinteger(L, 1);
            lua_setfield(L, target, cstr!("conf_id"));
        } else if let Some(route) = (*ctx).state.route {
            let lua = lua_of(ctx);
            let meta = if lua.merged.is_null() {
                &lua_gen(generation(ctx)).routes[route].meta
            } else {
                &(*lua.merged).meta
            };
            for (field, reference) in META_FIELDS.iter().zip(meta.0) {
                push_ref(L, reference);
                lua_setfield(L, target, field.as_ptr() as *const c_char);
            }
        } else {
            for field in &META_FIELDS[..3] {
                lua_pushnil(L);
                lua_setfield(L, target, field.as_ptr() as *const c_char);
            }
        }
        pop(L, 1);
    }
}

// ----------------------------------------------------- variables and HTTP

/// `ac_native_var_write` without `native_var_writes` (off in the measured
/// C variant): ngx_lua's exported variable setter.
pub(crate) unsafe fn native_var_set(L: *mut lua_State, r: *mut ngx_http_request_t, name: &[u8], value: Option<&[u8]>) {
    let mut lower = [0u8; 128];
    let mut error = [0u8; 256];
    let mut error_len = error.len();
    if name.len() > lower.len() {
        unsafe { fail(L, b"native variable name too long") }
    }
    let (data, len) = value.map_or((ptr::null_mut(), 0), |value| (value.as_ptr() as *mut u8, value.len()));
    let status = unsafe {
        ngx_http_lua_ffi_var_set(
            r,
            name.as_ptr() as *mut u8,
            name.len(),
            lower.as_mut_ptr(),
            data,
            len,
            error.as_mut_ptr(),
            &mut error_len,
        )
    };
    if status != NGX_OK as c_int {
        unsafe { fail(L, &error[..error_len.min(error.len())]) }
    }
}

/// `ac_var_assign`: optional write-through plus the `ctx.var._cache` entry.
unsafe fn var_assign(L: *mut lua_State, table: c_int, r: *mut ngx_http_request_t, name: &[u8], value: Option<&[u8]>, through: bool) {
    unsafe {
        if through {
            native_var_set(L, r, name, value);
        }
        push_ref(L, table);
        lua_getfield(L, -1, cstr!("var"));
        lua_getfield(L, -1, cstr!("_cache"));
        push_bytes(L, name);
        match value {
            Some(value) => push_bytes(L, value),
            None => lua_pushnil(L),
        }
        lua_settable(L, -3);
        pop(L, 3);
    }
}

/// `ac_var_batch_begin`: the raw `ctx.var._cache` when it has no metatable.
unsafe fn batch_begin(L: *mut lua_State, table: c_int) -> c_int {
    unsafe {
        let top = lua_gettop(L);
        push_ref(L, table);
        push_bytes(L, b"var");
        lua_rawget(L, -2);
        if is_table(L, -1) {
            push_bytes(L, b"_cache");
            lua_rawget(L, -2);
            if is_table(L, -1) && lua_getmetatable(L, -1) == 0 {
                return lua_gettop(L);
            }
        }
        lua_settop(L, top);
        0
    }
}

unsafe fn batch_assign(
    L: *mut lua_State,
    table: c_int,
    cache: c_int,
    r: *mut ngx_http_request_t,
    name: &[u8],
    value: Option<&[u8]>,
    through: bool,
) {
    unsafe {
        if cache == 0 {
            return var_assign(L, table, r, name, value, through);
        }
        if through {
            native_var_set(L, r, name, value);
        }
        push_bytes(L, name);
        match value {
            Some(value) => push_bytes(L, value),
            None => lua_pushnil(L),
        }
        lua_settable(L, cache);
    }
}

unsafe fn batch_end(L: *mut lua_State, cache: c_int) {
    if cache != 0 {
        unsafe { lua_settop(L, cache - 3) }
    }
}

#[inline]
fn hash_of(name: &[u8]) -> ngx_uint_t {
    name.iter().fold(0, |key: ngx_uint_t, &byte| key.wrapping_mul(31).wrapping_add(byte as ngx_uint_t))
}

/// `ac_read_var` (http_var_plan off): custom getter check, then the named
/// NGINX variable. Returned bytes live in the request pool.
unsafe fn read_var<'a>(L: *mut lua_State, table: c_int, r: *mut ngx_http_request_t, name: &[u8]) -> Option<&'a [u8]> {
    unsafe {
        push_ref(L, (*engine()).var_getters_ref);
        let mut custom = false;
        if is_table(L, -1) {
            push_bytes(L, name);
            lua_gettable(L, -2);
            custom = is_table(L, -1);
            pop(L, 1);
        }
        pop(L, 1);
        if !custom {
            let mut key = ngx_str_t { len: name.len(), data: name.as_ptr() as *mut u8 };
            let value = ngx_http_get_variable(r, &mut key, hash_of(name));
            if value.is_null() || (*value).not_found() != 0 {
                return None;
            }
            let len = (*value).len() as usize;
            return Some(if len == 0 { &[] } else { core::slice::from_raw_parts((*value).data, len) });
        }
        push_ref(L, table);
        lua_getfield(L, -1, cstr!("var"));
        push_bytes(L, name);
        lua_gettable(L, -2);
        let result = match to_bytes(L, -1) {
            None => None,
            Some(value) => {
                let copy = http::pool_bytes(r, value.len())?;
                copy.copy_from_slice(value);
                Some(&*copy)
            }
        };
        pop(L, 3);
        result
    }
}

const FORWARDED: [(&[u8], &[u8], &[u8]); 4] = [
    (b"X-Forwarded-Proto", b"original_x_forwarded_proto", b"http_x_forwarded_proto"),
    (b"X-Forwarded-Host", b"original_x_forwarded_host", b"http_x_forwarded_host"),
    (b"X-Forwarded-Port", b"original_x_forwarded_port", b"http_x_forwarded_port"),
    (b"Forwarded", b"original_forwarded", b"http_forwarded"),
];

/// `ac_forwarded` without trusted addresses (rejected at compile time).
unsafe fn forwarded(L: *mut lua_State, table: c_int, r: *mut ngx_http_request_t) {
    unsafe {
        for (_, original, variable) in FORWARDED {
            let value = read_var(L, table, r, variable);
            var_assign(L, table, r, original, Some(value.unwrap_or_default()), true);
        }
        if let Some(inbound) = read_var(L, table, r, b"http_x_forwarded_for") {
            var_assign(L, table, r, b"original_x_forwarded_for", Some(inbound), true);
        }
        let scheme = read_var(L, table, r, b"scheme");
        let host = read_var(L, table, r, b"http_host");
        let host_value = match host {
            Some(host) if !host.is_empty() => Some(host),
            _ => read_var(L, table, r, b"host"),
        };
        let mut port = read_var(L, table, r, b"server_port");
        if let Some(host) = host {
            if let Some(colon) = host.iter().rposition(|&b| b == b':') {
                let digits = &host[colon + 1..];
                if !digits.is_empty() && digits.iter().all(u8::is_ascii_digit) {
                    port = Some(digits);
                }
            }
        }
        let safe = [scheme, host_value, port, None];
        let cache = batch_begin(L, table);
        for (index, (header, _, variable)) in FORWARDED.iter().enumerate() {
            sdk::native_header_set(L, r, header, safe[index], false, false);
            batch_assign(L, table, cache, r, variable, safe[index], false);
        }
        batch_end(L, cache);
    }
}

/// `ac_http_prepare` with the measured C switches (no servlet/encoded-slash
/// normalization: rejected at compile time).
unsafe fn http_prepare(L: *mut lua_State, ctx: *mut RequestCtx, r: *mut ngx_http_request_t) -> u16 {
    unsafe {
        let table = lua_of(ctx).table_ref;
        let uri = http::bytes(&(*r).uri);
        let raw = http::bytes(&(*r).unparsed_uri);
        let path = match raw.iter().position(|&b| b == b'?') {
            Some(query) => &raw[..query],
            None => raw,
        };
        let mut normalized = uri;
        if generation(ctx).core.settings.delete_tail_slash && normalized.last() == Some(&b'/') {
            normalized = &normalized[..normalized.len() - 1];
        }
        let Some(scratch) = http::pool_bytes(r, path.len() + 1) else { fail(L, b"URI decoding allocation failed") };
        if !only_decoded_or_merged(path, uri, scratch) {
            ngx_member(L, Some(cstr!("req")), Some(cstr!("set_uri")));
            push_bytes(L, uri);
            lua_pushboolean(L, 0);
            lua_pushboolean(L, 1);
            lua_call(L, 3, 0);
        }
        lua_of(ctx).normalized = http::str_of(normalized);
        let args = http::bytes(&(*r).args);
        let Some(joined) = http::pool_bytes(r, normalized.len() + args.len() + 1) else {
            fail(L, b"normalized request URI allocation failed")
        };
        joined[..normalized.len()].copy_from_slice(normalized);
        let mut joined_len = normalized.len();
        if !args.is_empty() {
            joined[joined_len] = b'?';
            joined[joined_len + 1..joined_len + 1 + args.len()].copy_from_slice(args);
            joined_len += args.len() + 1;
        }
        let cache = batch_begin(L, table);
        batch_assign(L, table, cache, r, b"uri", Some(normalized), false);
        batch_assign(L, table, cache, r, b"real_request_uri", Some(raw), false);
        batch_assign(L, table, cache, r, b"request_uri", Some(&joined[..joined_len]), false);
        batch_assign(L, table, cache, r, b"request_line", Some(http::bytes(&(*r).request_line)), true);
        batch_end(L, cache);
        forwarded(L, table, r);
        var_assign(L, table, r, b"uri", Some(normalized), false);
        0
    }
}

/// `ac_match_start` + `ac_match_next` + `ac_accept_route` (lazy route
/// options: no options table unless a route filter exists, which the
/// closed-world compile rejects).
unsafe fn match_route(L: *mut lua_State, ctx: *mut RequestCtx, r: *mut ngx_http_request_t) {
    unsafe {
        let table = lua_of(ctx).table_ref;
        let generation = generation(ctx);
        let core = &generation.core;
        // Observable even without a filter: publishes ctx.var.remote_addr.
        push_ref(L, table);
        lua_getfield(L, -1, cstr!("var"));
        lua_getfield(L, -1, cstr!("remote_addr"));
        pop(L, 3);
        let normalized = http::bytes(&lua_of(ctx).normalized);
        let host = (*r).headers_in.host;
        let server = http::bytes(&(*r).headers_in.server);
        let host_header = if host.is_null() { None } else { Some(http::bytes(&(*host).value)) };
        let method = (*r).method as u32;
        let sockaddr = (*(*r).connection).sockaddr;
        let host_router = core.settings.host_router;
        let found = core.router.find(normalized, |index| {
            let candidate = &core.routes[index];
            let (rank, _) = host_match(&candidate.hosts, host_router, server, host_header)?;
            if candidate.methods != 0 && candidate.methods & method == 0 {
                return None;
            }
            let cidrs = generation.cidrs[index];
            if !cidrs.is_null() && ngx_cidr_match(sockaddr, cidrs) != NGX_OK as ngx_int_t {
                return None;
            }
            Some(if host_router { rank } else { 0 })
        });
        if let Some((index, path)) = found {
            let route = &core.routes[index];
            let lua_route = &lua_gen(generation).routes[index];
            (*ctx).state.route = Some(index);
            lua_of(ctx).early_status = route.resolve_status;
            push_ref(L, table);
            let target = lua_gettop(L);
            lua_createtable(L, 0, 3);
            let matched = lua_gettop(L);
            push_bytes(L, core.router.path_source(index, path));
            lua_setfield(L, matched, cstr!("_path"));
            if let Some((_, host)) = host_match(&route.hosts, host_router, server, host_header) {
                if !route.hosts.is_empty() {
                    push_bytes(L, &route.hosts[host]);
                    lua_setfield(L, matched, cstr!("_host"));
                }
            }
            push_str(L, &(*r).method_name);
            lua_setfield(L, matched, cstr!("_method"));
            lua_pushvalue(L, matched);
            lua_setfield(L, target, cstr!("curr_req_matched"));
            push_ref(L, lua_route.wrapper_ref);
            lua_setfield(L, target, cstr!("matched_route"));
            push_ref(L, lua_route.value_ref);
            lua_getfield(L, -1, cstr!("id"));
            lua_setfield(L, target, cstr!("route_id"));
            lua_settop(L, target - 1);
            route_context(L, ctx, false);
        }
        var_assign(L, table, r, b"uri", Some(normalized), false);
    }
}

// ------------------------------------------------------ consumer merging

/// Copies a route wrapper, sharing every plugin configuration table
/// (`deepcopy` with `shallow_prefix = "self.value.plugins"`).
unsafe fn deepcopy(L: *mut lua_State, source: c_int, copied: c_int, level: u8, depth: usize) {
    unsafe {
        let source = absolute(L, source);
        if !is_table(L, source) {
            lua_pushvalue(L, source);
            return;
        }
        if depth > 256 {
            fail(L, b"table nesting exceeds maximum depth");
        }
        lua_pushvalue(L, source);
        lua_rawget(L, copied);
        if !is_nil(L, -1) {
            return;
        }
        pop(L, 1);
        new_table(L);
        let dest = lua_gettop(L);
        lua_pushvalue(L, source);
        lua_pushvalue(L, dest);
        lua_rawset(L, copied);
        lua_pushnil(L);
        while lua_next(L, source) != 0 {
            // level: 0 = self, 1 = self.value, 2 = self.value.plugins.
            let key = string_bytes(L, -2);
            let child = match (level, key) {
                (0, Some(b"value")) => 1,
                (1, Some(b"plugins")) => 2,
                (2, _) => 3,
                _ => 4,
            };
            lua_pushvalue(L, -2);
            if child == 3 {
                lua_pushvalue(L, -2);
            } else {
                deepcopy(L, -2, copied, child, depth + 1);
            }
            lua_rawset(L, dest);
            pop(L, 1);
        }
        if lua_getmetatable(L, source) != 0 {
            lua_setmetatable(L, dest);
        }
    }
}

unsafe fn count_keys(L: *mut lua_State, table: c_int) -> usize {
    unsafe {
        let table = absolute(L, table);
        if !is_table(L, table) {
            return 0;
        }
        let mut count = 0;
        lua_pushnil(L);
        while lua_next(L, table) != 0 {
            count += 1;
            pop(L, 1);
        }
        count
    }
}

/// Metadata with the consumer suffixes of `ac_route_context_at`.
unsafe fn consumer_meta(L: *mut lua_State, table: c_int, base: &Meta, refs: &mut Vec<c_int>) -> Meta {
    unsafe {
        push_ref(L, table);
        let target = lua_gettop(L);
        let mut fields = [LUA_NOREF; 4];
        // conf_type
        push_ref(L, base.0[0]);
        push_bytes(L, b"&consumer");
        lua_concat(L, 2);
        fields[0] = luaL_ref(L, LUA_REGISTRYINDEX);
        // conf_id = conf_id .. "&" .. consumer_name
        push_ref(L, base.0[1]);
        push_bytes(L, b"&");
        lua_getfield(L, target, cstr!("consumer_name"));
        lua_concat(L, 3);
        fields[1] = luaL_ref(L, LUA_REGISTRYINDEX);
        // conf_version = conf_version .. "&" .. consumer_ver
        push_ref(L, base.0[2]);
        push_bytes(L, b"&");
        lua_getfield(L, target, cstr!("consumer_ver"));
        lua_concat(L, 3);
        fields[2] = luaL_ref(L, LUA_REGISTRYINDEX);
        push_ref(L, base.0[3]);
        fields[3] = luaL_ref(L, LUA_REGISTRYINDEX);
        pop(L, 1);
        refs.extend_from_slice(&fields);
        Meta(fields)
    }
}

/// `ac_consumer_merge` + `ac_consumer_build`, cached per generation.
unsafe fn consumer_merge(L: *mut lua_State, ctx: *mut RequestCtx) -> bool {
    unsafe {
        let top = lua_gettop(L);
        let table = lua_of(ctx).table_ref;
        let Some(route) = (*ctx).state.route else { return false };
        let generation = generation(ctx);
        let luagen = lua_gen(generation);
        push_ref(L, table);
        lua_getfield(L, -1, cstr!("consumer"));
        let consumer = lua_gettop(L);
        if !is_table(L, consumer) {
            lua_settop(L, top);
            return false;
        }
        lua_of(ctx).merged = ptr::null();
        lua_getfield(L, consumer, cstr!("group_id"));
        if !is_nil(L, -1) {
            fail(L, b"consumer groups are not supported by the Rust step-1 core");
        }
        pop(L, 1);
        let key = lua_topointer(L, consumer);
        let found = luagen.merged.borrow().iter().find(|entry| entry.route == route && entry.consumer == key).map(|e| &**e as *const Merged);
        let entry = match found {
            Some(entry) => entry,
            None => {
                let entry = build_merged(L, ctx, route, consumer, key);
                let mut cache = luagen.merged.borrow_mut();
                if cache.len() < MERGE_CACHE {
                    cache.push(entry);
                    cache.last().map_or(ptr::null(), |entry| &**entry as *const Merged)
                } else {
                    drop(cache);
                    let lua = lua_of(ctx);
                    if let Some(previous) = lua.owned.take() {
                        for reference in &previous.refs {
                            unref(L, *reference);
                        }
                    }
                    lua.owned = Some(entry);
                    lua.owned.as_deref().map_or(ptr::null(), |entry| entry as *const Merged)
                }
            }
        };
        lua_of(ctx).merged = entry;
        if (*entry).changed {
            push_ref(L, (*entry).wrapper_ref);
            lua_setfield(L, top + 1, cstr!("matched_route"));
        }
        lua_settop(L, top);
        route_context(L, ctx, false);
        (*entry).changed
    }
}

unsafe fn build_merged(L: *mut lua_State, ctx: *mut RequestCtx, route: usize, consumer: c_int, key: *const c_void) -> Box<Merged> {
    unsafe {
        let table = lua_of(ctx).table_ref;
        let luagen = lua_gen(generation(ctx));
        let base = &luagen.routes[route];
        let mut refs = Vec::new();
        lua_getfield(L, consumer, cstr!("plugins"));
        let count = count_keys(L, -1);
        pop(L, 1);
        let meta = consumer_meta(L, table, &base.meta, &mut refs);
        if count == 0 {
            return Box::new(Merged { route, consumer: key, changed: false, wrapper_ref: LUA_NOREF, calls: None, meta, refs });
        }
        new_table(L);
        let copied = lua_gettop(L);
        push_ref(L, base.wrapper_ref);
        deepcopy(L, -1, copied, 0, 0);
        let wrapper = lua_gettop(L);
        lua_getfield(L, wrapper, cstr!("value"));
        let value = lua_gettop(L);
        lua_getfield(L, value, cstr!("plugins"));
        if is_nil(L, -1) {
            pop(L, 1);
            new_table(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, value, cstr!("plugins"));
        }
        let plugins = lua_gettop(L);
        lua_getfield(L, consumer, cstr!("plugins"));
        let source = lua_gettop(L);
        if is_table(L, source) {
            lua_pushnil(L);
            while lua_next(L, source) != 0 {
                lua_pushvalue(L, -2);
                lua_rawget(L, plugins);
                if is_nil(L, -1) {
                    lua_pushboolean(L, 1);
                    lua_setfield(L, -3, cstr!("_from_consumer"));
                }
                pop(L, 1);
                lua_pushvalue(L, -2);
                lua_pushvalue(L, -2);
                lua_rawset(L, plugins);
                pop(L, 1);
            }
        }
        pop(L, 1);
        let mut calls = compile_plan(L, plugins, false, &mut refs);
        for call in &mut calls {
            push_ref(L, call.conf_ref);
            lua_getfield(L, -1, cstr!("_from_consumer"));
            call.consumer_only = truthy(L, -1);
            pop(L, 1);
            let auth = loaded()[call.plugin].auth;
            if !call.consumer_only || auth {
                lua_pushboolean(L, 1);
                lua_setfield(L, -2, cstr!("_skip_rewrite_in_consumer"));
            }
            pop(L, 1);
        }
        let wrapper_ref = reference(L, wrapper);
        refs.push(wrapper_ref);
        lua_settop(L, copied - 1);
        Box::new(Merged { route, consumer: key, changed: true, wrapper_ref, calls: Some(calls), meta, refs })
    }
}

/// `ac_compile_plan` (no filters or pre-functions: rejected at compile time).
unsafe fn compile_plan(L: *mut lua_State, plugins: c_int, validate: bool, refs: &mut Vec<c_int>) -> Vec<Call> {
    unsafe {
        let plugins = absolute(L, plugins);
        let mut calls = Vec::new();
        if is_nil(L, plugins) {
            return calls;
        }
        luaL_checktype(L, plugins, LUA_TTABLE);
        lua_pushnil(L);
        while lua_next(L, plugins) != 0 {
            let name = check_bytes(L, -2);
            let Some(index) = loaded().iter().position(|plugin| plugin.name == name) else {
                let mut message = b"plugin '".to_vec();
                message.extend_from_slice(name);
                message.extend_from_slice(b"' is not enabled");
                fail(L, &message)
            };
            luaL_checktype(L, -1, LUA_TTABLE);
            lua_getfield(L, -1, cstr!("_meta"));
            let meta = lua_gettop(L);
            let mut priority = loaded()[index].priority;
            if is_table(L, meta) {
                lua_getfield(L, meta, cstr!("disable"));
                let disabled = truthy(L, -1);
                pop(L, 1);
                if disabled {
                    pop(L, 2);
                    continue;
                }
                lua_getfield(L, meta, cstr!("priority"));
                if !is_nil(L, -1) {
                    priority = luaL_checknumber(L, -1);
                }
                pop(L, 1);
            }
            let conf_ref = reference(L, -2);
            refs.push(conf_ref);
            if validate {
                push_ref(L, loaded()[index].module_ref);
                lua_getfield(L, -1, cstr!("check_schema"));
                if is_function(L, -1) {
                    push_ref(L, conf_ref);
                    if lua_pcall(L, 1, 2, 0) != 0 {
                        let mut message = b"plugin schema raised: ".to_vec();
                        message.extend_from_slice(to_bytes(L, -1).unwrap_or(b"non-string error"));
                        fail(L, &message);
                    }
                    if !truthy(L, -2) {
                        let mut message = b"invalid plugin configuration: ".to_vec();
                        message.extend_from_slice(to_bytes(L, -1).unwrap_or(b"unknown error"));
                        fail(L, &message);
                    }
                    pop(L, 2);
                } else {
                    pop(L, 1);
                }
                pop(L, 1);
            }
            calls.push(Call { plugin: index, conf_ref, priority, consumer_only: false });
            pop(L, 2);
        }
        calls.sort_by(|left, right| right.priority.total_cmp(&left.priority));
        calls
    }
}

// --------------------------------------------------------------- upstream

/// `ac_upstream_prepare` (Lua path): proxied scheme/host/URI, the
/// upstream conf view and the first peer.
unsafe fn upstream_prepare(L: *mut lua_State, ctx: *mut RequestCtx, r: *mut ngx_http_request_t) -> Option<u16> {
    unsafe {
        let generation = generation(ctx);
        let table = lua_of(ctx).table_ref;
        let route = (*ctx).state.route?;
        let Some(upstream) = generation.core.routes[route].upstream.as_ref() else { return Some(503) };
        if upstream.nodes.is_empty() {
            return Some(503);
        }
        push_ref(L, table);
        lua_getfield(L, -1, cstr!("upstream_scheme"));
        let scheme: Vec<u8> = string_bytes(L, -1).map_or_else(|| b"http".to_vec(), <[u8]>::to_vec);
        pop(L, 2);
        let host: &[u8] = match upstream.rewrite_host.as_deref() {
            Some(host) => host,
            None => {
                let header = (*r).headers_in.host;
                if header.is_null() {
                    http::bytes(&(*r).headers_in.server)
                } else {
                    http::bytes(&(*header).value)
                }
            }
        };
        let uri = if (*r).valid_unparsed_uri() != 0 {
            (*r).unparsed_uri
        } else {
            let args = http::bytes(&(*r).args);
            let suffix = if args.is_empty() { None } else { Some(args) };
            match http::escaped(r, http::bytes(&(*r).uri), NGX_ESCAPE_URI, suffix) {
                Some(uri) => uri,
                None => fail(L, b"proxy URI allocation failed"),
            }
        };
        push_ref(L, table);
        push_ref(L, lua_gen(generation).routes[route].upstream_conf_ref);
        lua_setfield(L, -2, cstr!("upstream_conf"));
        pop(L, 1);
        if (*ctx).state.peer.is_none() {
            let now = ngx_current_msec as u64;
            let mut peer = Peer::new(upstream.nodes.len(), now);
            let Some(node) = upstream.pick(&mut peer, now) else { return Some(502) };
            (*ctx).state.peer = Some(peer);
            publish_peer(L, ctx, r, route, node);
        }
        let cache = batch_begin(L, table);
        batch_assign(L, table, cache, r, b"upstream_uri", Some(http::bytes(&uri)), true);
        batch_assign(L, table, cache, r, b"upstream_scheme", Some(&scheme), true);
        batch_assign(L, table, cache, r, b"upstream_host", Some(host), true);
        batch_end(L, cache);
        None
    }
}

/// The `ac_peer_get` Lua-path publication: balancer_ip/port and the
/// unresolved host on api_ctx and in `ctx.var`.
unsafe fn publish_peer(L: *mut lua_State, ctx: *mut RequestCtx, r: *mut ngx_http_request_t, route: usize, node: usize) {
    unsafe {
        let generation = generation(ctx);
        let table = lua_of(ctx).table_ref;
        let address = &generation.peers[route][node];
        let upstream_node = match generation.core.routes[route].upstream.as_ref() {
            Some(upstream) => &upstream.nodes[node],
            None => return,
        };
        let mut ip = [0u8; SOCKADDR_STRLEN];
        let len = ngx_sock_ntop(address.sockaddr, address.socklen, ip.as_mut_ptr(), ip.len(), 0);
        push_ref(L, table);
        push_bytes(L, &ip[..len]);
        lua_setfield(L, -2, cstr!("balancer_ip"));
        lua_pushinteger(L, upstream_node.port as isize);
        lua_setfield(L, -2, cstr!("balancer_port"));
        push_bytes(L, &upstream_node.host);
        lua_setfield(L, -2, cstr!("upstream_unresolved_host"));
        pop(L, 1);
        var_assign(L, table, r, b"upstream_unresolved_host", Some(&upstream_node.host), true);
    }
}

/// `ac_balance_enter_ctx`: false for the first attempt (peer chosen in
/// access), true after re-selecting for a retry; `false, 502` on failure.
unsafe fn balance_enter(L: *mut lua_State, ctx: *mut RequestCtx, r: *mut ngx_http_request_t) -> c_int {
    unsafe {
        let generation = generation(ctx);
        let state = &mut (*ctx).state;
        let (Some(route), Some(peer)) = (state.route, state.peer.as_mut()) else {
            lua_pushboolean(L, 0);
            lua_pushinteger(L, 502);
            return 2;
        };
        let Some(upstream) = generation.core.routes[route].upstream.as_ref() else {
            lua_pushboolean(L, 0);
            lua_pushinteger(L, 502);
            return 2;
        };
        let attempts = state.balance_attempts;
        if attempts > 0 {
            let now = ngx_current_msec as u64;
            if upstream.retry_timeout_ms > 0 && now.wrapping_sub(peer.started_ms) > upstream.retry_timeout_ms {
                lua_pushboolean(L, 0);
                lua_pushinteger(L, 502);
                return 2;
            }
            let mut status: c_int = 0;
            let mut error: *mut c_char = ptr::null_mut();
            let failed = ngx_http_lua_ffi_balancer_get_last_failure(r, &mut status, &mut error) > 0;
            upstream.release(peer, failed, now);
            let Some(node) = upstream.pick(peer, now) else {
                lua_pushboolean(L, 0);
                lua_pushinteger(L, 502);
                return 2;
            };
            publish_peer(L, ctx, r, route, node);
        }
        lua_pushboolean(L, (attempts > 0) as c_int);
        1
    }
}

/// CRC-32 (IEEE), as `ngx_crc32_long` for the keepalive pool key.
fn crc32(data: &[u8]) -> u32 {
    const TABLE: [u32; 256] = {
        let mut table = [0u32; 256];
        let mut index = 0;
        while index < 256 {
            let mut value = index as u32;
            let mut bit = 0;
            while bit < 8 {
                value = if value & 1 != 0 { 0xedb8_8320 ^ (value >> 1) } else { value >> 1 };
                bit += 1;
            }
            table[index] = value;
            index += 1;
        }
        table
    };
    let mut crc = 0xffff_ffffu32;
    for &byte in data {
        crc = TABLE[((crc ^ u32::from(byte)) & 0xff) as usize] ^ (crc >> 8);
    }
    crc ^ 0xffff_ffff
}

unsafe fn balance_failed(L: *mut lua_State, what: &[u8], error: *mut c_char) -> ! {
    let mut message = what.to_vec();
    if !error.is_null() {
        message.extend_from_slice(b": ");
        message.extend_from_slice(unsafe { core::ffi::CStr::from_ptr(error) }.to_bytes());
    }
    unsafe { fail(L, &message) }
}

/// `ac_balance_finish_ctx`: retries/timeouts on the first attempt, the
/// selected peer and keepalive pool through ngx_lua's balancer API.
unsafe fn balance_finish_ctx(L: *mut lua_State, ctx: *mut RequestCtx, r: *mut ngx_http_request_t) -> c_int {
    unsafe {
        let generation = generation(ctx);
        let state = &mut (*ctx).state;
        let selected = state.peer.as_ref().and_then(|peer| peer.selected);
        let (Some(route), Some(node)) = (state.route, selected) else {
            lua_pushinteger(L, 502);
            return 1;
        };
        let Some(upstream) = generation.core.routes[route].upstream.as_ref() else {
            lua_pushinteger(L, 502);
            return 1;
        };
        let mut error: *mut c_char = ptr::null_mut();
        if state.balance_attempts == 0 {
            let retries = upstream.effective_retries();
            if retries > 0 && ngx_http_lua_ffi_balancer_set_more_tries(r, retries as c_int, &mut error) != NGX_OK as c_int {
                balance_failed(L, b"failed to set native retry count", error);
            }
            if upstream.connect_timeout_ms == 0 || upstream.send_timeout_ms == 0 || upstream.read_timeout_ms == 0 {
                fail(L, b"native upstream timeouts must be positive");
            }
            if ngx_http_lua_ffi_balancer_set_timeouts(
                r,
                upstream.connect_timeout_ms as core::ffi::c_long,
                upstream.send_timeout_ms as core::ffi::c_long,
                upstream.read_timeout_ms as core::ffi::c_long,
                &mut error,
            ) != NGX_OK as c_int
            {
                balance_failed(L, b"failed to set native timeouts", error);
            }
        } else if ngx_http_lua_ffi_balancer_recreate_request(r, &mut error) != NGX_OK as c_int {
            balance_failed(L, b"failed to recreate proxy request", error);
        }
        let address = &generation.peers[route][node];
        let port = upstream.nodes[node].port;
        let mut ip = [0u8; SOCKADDR_STRLEN + 2];
        let len = ngx_sock_ntop(address.sockaddr, address.socklen, ip.as_mut_ptr().add(1), ip.len() - 2, 0);
        let scheme = state.vars.get(crate::vars::UPSTREAM_SCHEME).unwrap_or(b"http");
        let mut key = Vec::with_capacity(scheme.len() + len + 8);
        key.extend_from_slice(scheme);
        key.push(b'#');
        key.extend_from_slice(&ip[1..1 + len]);
        key.push(b'#');
        key.extend_from_slice(port.to_string().as_bytes());
        let pool_key = crc32(&key);
        // The runtime parses a URL-style address: IPv6 needs brackets.
        let peer: &[u8] = if (*address.sockaddr).sa_family as i32 == AF_INET6 as i32 {
            ip[0] = b'[';
            ip[1 + len] = b']';
            &ip[..len + 2]
        } else {
            &ip[1..1 + len]
        };
        if ngx_http_lua_ffi_balancer_set_current_peer(r, peer.as_ptr(), peer.len(), c_int::from(port), pool_key, KEEPALIVE_SIZE, &mut error)
            != NGX_OK as c_int
        {
            balance_failed(L, b"failed to set native current peer", error);
        }
        if ngx_http_lua_ffi_balancer_enable_keepalive(r, KEEPALIVE_IDLE_MS as core::ffi::c_ulong, KEEPALIVE_REQUESTS, &mut error)
            != NGX_OK as c_int
        {
            balance_failed(L, b"failed to enable native keepalive", error);
        }
        (*ctx).state.balance_attempts += 1;
        0
    }
}

unsafe extern "C-unwind" fn balance_start(L: *mut lua_State) -> c_int {
    unsafe {
        let ctx = context(L);
        let r = request(L);
        balance_enter(L, ctx, r)
    }
}

unsafe extern "C-unwind" fn balance_finish(L: *mut lua_State) -> c_int {
    unsafe {
        let ctx = context(L);
        let r = request(L);
        balance_finish_ctx(L, ctx, r)
    }
}

/// `ac_upstream_log`: peer release and EWMA feedback.
unsafe fn upstream_log(ctx: *mut RequestCtx, r: *mut ngx_http_request_t) {
    unsafe {
        let lua = lua_of(ctx);
        if lua.upstream_logged {
            return;
        }
        lua.upstream_logged = true;
        let generation = generation(ctx);
        let state = &mut (*ctx).state;
        let Some(upstream) = state.route.and_then(|route| generation.core.routes[route].upstream.as_ref()) else { return };
        let u = (*r).upstream;
        let last = if u.is_null() { ptr::null_mut() } else { (*u).state };
        if let Some(peer) = state.peer.as_mut() {
            let failed = !last.is_null() && ((*last).status == 502 || (*last).status == 504);
            upstream.release(peer, failed, ngx_current_msec as u64);
        }
        if !last.is_null() && !(*last).peer.is_null() {
            upstream.record_response_time(http::bytes(&*(*last).peer), (*last).response_time as f64);
        }
    }
}

/// `ac_render`: JSON-encodes table bodies, marks the response source.
unsafe extern "C-unwind" fn render(L: *mut lua_State) -> c_int {
    unsafe {
        let status = luaL_checknumber(L, 1) as isize;
        let ctx = context(L);
        lua_settop(L, 2);
        push_ref(L, lua_of(ctx).table_ref);
        lua_getfield(L, -1, cstr!("_resp_source"));
        if !truthy(L, -1) {
            push_bytes(L, b"apisix");
            lua_setfield(L, -3, cstr!("_resp_source"));
        }
        pop(L, 2);
        if is_table(L, 2) {
            get_global(L, cstr!("require"));
            push_bytes(L, b"cjson.safe");
            lua_call(L, 1, 1);
            lua_getfield(L, -1, cstr!("encode"));
            lua_pushvalue(L, 2);
            lua_call(L, 1, 2);
            if is_nil(L, -2) {
                let mut message = b"native response JSON encoding failed: ".to_vec();
                message.extend_from_slice(to_bytes(L, -1).unwrap_or_default());
                fail(L, &message);
            }
            pop(L, 1);
            push_bytes(L, b"\n");
            lua_concat(L, 2);
            lua_replace(L, 2);
            lua_settop(L, 2);
            ngx_member(L, Some(cstr!("header")), None);
            push_bytes(L, b"application/json");
            lua_setfield(L, -2, cstr!("Content-Type"));
            pop(L, 1);
        }
        lua_pushinteger(L, status);
        lua_pushvalue(L, 2);
        2
    }
}

// ------------------------------------------------- init, worker, reload

unsafe fn json_field<'a>(root: &'a Value, key: &str) -> Option<&'a Value> {
    root.get(key).filter(|value| !value.is_null())
}

/// `engine.init(path)` in init_by_lua (master): validates the standalone
/// configuration with the Rust compiler and enables the privileged agent.
unsafe extern "C-unwind" fn init(L: *mut lua_State) -> c_int {
    unsafe {
        let path = check_bytes(L, 1);
        let engine = engine();
        if (*engine).initialized {
            fail(L, b"native core is already initialized");
        }
        if lua_pushthread(L) == 0 {
            fail(L, b"native core must initialize on the main Lua VM");
        }
        pop(L, 1);
        let Ok(path) = std::str::from_utf8(path) else { fail(L, b"configuration path is not UTF-8") };
        let path = path.to_owned();
        let bytes = match runtime::read_source(&path) {
            Ok(bytes) => bytes,
            Err(error) => fail(L, error.as_bytes()),
        };
        let root: Value = match serde_json::from_slice(&bytes) {
            Ok(root) => root,
            Err(error) => fail(L, format!("invalid standalone JSON: {error}").as_bytes()),
        };
        if let Err(error) = apisix_rs_core::config::Generation::compile(&root) {
            fail(L, format!("standalone configuration rejected: {error}").as_bytes());
        }
        (*engine).vm = L;
        (*engine).path = path;
        require(L, b"ngx.process");
        lua_getfield(L, -1, cstr!("enable_privileged_agent"));
        lua_call(L, 0, 2);
        if !truthy(L, -2) {
            let mut message = b"native privileged worker setup failed: ".to_vec();
            message.extend_from_slice(to_bytes(L, -1).unwrap_or_default());
            fail(L, &message);
        }
        pop(L, 3);
        (*engine).initialized = true;
        lua_pushboolean(L, 1);
        1
    }
}

/// `ac_compile_plugins`: loads the enabled plugin modules.
unsafe fn load_plugins(L: *mut lua_State, root: &Value) {
    unsafe {
        let engine = engine();
        let names: Vec<Vec<u8>> = json_field(root, "plugins")
            .and_then(Value::as_array)
            .map(|list| list.iter().filter_map(Value::as_str).map(|name| name.as_bytes().to_vec()).collect())
            .unwrap_or_default();
        for name in names {
            if loaded().iter().any(|plugin| plugin.name == name) {
                fail(L, b"duplicate enabled plugin");
            }
            get_global(L, cstr!("require"));
            push_bytes(L, b"apisix.plugins.");
            push_bytes(L, &name);
            lua_concat(L, 2);
            if lua_pcall(L, 1, 1, 0) != 0 {
                let mut message = b"failed loading plugin: ".to_vec();
                message.extend_from_slice(to_bytes(L, -1).unwrap_or_default());
                fail(L, &message);
            }
            let module = lua_gettop(L);
            luaL_checktype(L, module, LUA_TTABLE);
            push_bytes(L, &name);
            lua_setfield(L, module, cstr!("name"));
            lua_getfield(L, module, cstr!("schema"));
            if is_table(L, -1) {
                let schema = lua_gettop(L);
                lua_getfield(L, schema, cstr!("properties"));
                if is_nil(L, -1) {
                    pop(L, 1);
                    new_table(L);
                    lua_pushvalue(L, -1);
                    lua_setfield(L, schema, cstr!("properties"));
                }
                require(L, b"apisix.schema_def");
                lua_getfield(L, -1, cstr!("plugin_injected_schema"));
                lua_getfield(L, -1, cstr!("_meta"));
                lua_setfield(L, -4, cstr!("_meta"));
                lua_getfield(L, -1, cstr!("$comment"));
                lua_setfield(L, schema, cstr!("$comment"));
                pop(L, 3);
            }
            pop(L, 1);
            lua_getfield(L, module, cstr!("priority"));
            let priority = luaL_checknumber(L, -1);
            pop(L, 1);
            if loaded().iter().any(|plugin| plugin.priority == priority) {
                fail(L, b"duplicate plugin priority");
            }
            lua_getfield(L, module, cstr!("type"));
            let auth = string_bytes(L, -1) == Some(b"auth".as_slice());
            pop(L, 1);
            let module_ref = reference(L, module);
            lua_getfield(L, module, cstr!("init"));
            if is_function(L, -1) {
                if lua_pcall(L, 0, 0, 0) != 0 {
                    fail(L, b"plugin init failed");
                }
            } else {
                pop(L, 1);
            }
            lua_getfield(L, module, cstr!("workflow_handler"));
            if is_function(L, -1) {
                if lua_pcall(L, 0, 0, 0) != 0 {
                    fail(L, b"plugin workflow registration failed");
                }
            } else {
                pop(L, 1);
            }
            let mut callbacks = [LUA_REFNIL; PHASES];
            for (phase, field) in PHASE_FIELDS.iter().enumerate() {
                lua_getfield(L, module, field.as_ptr() as *const c_char);
                if !is_nil(L, -1) && !is_function(L, -1) {
                    fail(L, b"invalid plugin callback");
                }
                callbacks[phase] = luaL_ref(L, LUA_REGISTRYINDEX);
            }
            // Native filters: no Lua header/body filter dispatch.
            for phase in [HEADER_FILTER as usize, BODY_FILTER as usize, BODY_FILTER as usize + 1] {
                if callbacks[phase] != LUA_REFNIL {
                    fail(L, b"native filter mode cannot dispatch Lua filter callbacks");
                }
            }
            pop(L, 1);
            (*engine).plugins.push(Plugin { name, module_ref, priority, auth, callbacks });
        }
    }
}

/// `engine.init_worker()`: plugins, the first generation and the watcher.
unsafe extern "C-unwind" fn init_worker(L: *mut lua_State) -> c_int {
    unsafe {
        let engine = engine();
        if !(*engine).initialized || (*engine).compiled {
            fail(L, b"native worker initialization requires one loaded configuration");
        }
        let path = (*engine).path.clone();
        let bytes = match runtime::read_source(&path) {
            Ok(bytes) => bytes,
            Err(error) => fail(L, error.as_bytes()),
        };
        let root: Value = match serde_json::from_slice(&bytes) {
            Ok(root) => root,
            Err(error) => fail(L, format!("invalid standalone JSON: {error}").as_bytes()),
        };
        load_plugins(L, &root);
        for index in 0..loaded().len() {
            push_ref(L, loaded()[index].module_ref);
            lua_getfield(L, -1, cstr!("init_worker"));
            if is_function(L, -1) {
                if lua_pcall(L, 0, 0, 0) != 0 {
                    fail(L, b"plugin init_worker failed");
                }
            } else {
                pop(L, 1);
            }
            pop(L, 1);
        }
        let generation = match build_generation(L, &root, None) {
            Ok(generation) => generation,
            Err(error) => fail(L, format!("standalone configuration rejected: {error}").as_bytes()),
        };
        publish(generation, bytes);
        (*engine).compiled = true;
        // The 1 s watcher runs as a Lua timer, like `ac_snapshot_worker`.
        ngx_member(L, Some(cstr!("timer")), Some(cstr!("every")));
        lua_pushinteger(L, 1);
        bind(L, tick, 0);
        lua_call(L, 2, 2);
        if !truthy(L, -2) {
            let mut message = b"native configuration watcher failed: ".to_vec();
            message.extend_from_slice(to_bytes(L, -1).unwrap_or_default());
            fail(L, &message);
        }
        0
    }
}

unsafe extern "C-unwind" fn shutdown(L: *mut lua_State) -> c_int {
    unsafe {
        let engine = engine();
        for index in 0..loaded().len() {
            push_ref(L, loaded()[index].module_ref);
            lua_getfield(L, -1, cstr!("destroy"));
            if is_function(L, -1) {
                if lua_pcall(L, 0, 0, 0) != 0 {
                    pop(L, 1);
                }
            } else {
                pop(L, 1);
            }
            pop(L, 1);
        }
        (*engine).exiting = true;
    }
    0
}

fn publish(generation: GenRt, bytes: Vec<u8>) {
    let lua = generation.lua.as_deref().map_or(ptr::null(), |lua| lua as *const LuaGen);
    // Dropping the previous generation clears `current` if it pointed there.
    runtime::publish(Rc::new(generation));
    unsafe {
        (*engine()).current = lua;
        (*engine()).source = bytes;
    }
}

/// Compiles the core generation and builds its Lua side in a protected call.
unsafe fn build_generation(L: *mut lua_State, root: &Value, previous: Option<&GenRt>) -> Result<GenRt, String> {
    let log = unsafe { (*ngx_cycle).log };
    let mut generation = runtime::compile_value(root, log, previous)?;
    let mut args = BuildArgs { root, generation: &generation, out: None, refs: Vec::new() };
    unsafe {
        let top = lua_gettop(L);
        push_fn(L, build_entry);
        lua_pushlightuserdata(L, &mut args as *mut BuildArgs as *mut c_void);
        if lua_pcall(L, 1, 0, 0) != 0 {
            let message = to_bytes(L, -1).map(|text| String::from_utf8_lossy(text).into_owned());
            lua_settop(L, top);
            for reference in args.refs.drain(..) {
                unref(L, reference);
            }
            return Err(message.unwrap_or_else(|| "non-string Lua error".into()));
        }
        lua_settop(L, top);
    }
    let mut lua = args.out.take().ok_or("Lua generation was not built")?;
    lua.refs = std::mem::take(&mut args.refs);
    generation.lua = Some(Box::new(lua));
    Ok(generation)
}

struct BuildArgs<'a> {
    root: &'a Value,
    generation: &'a GenRt,
    out: Option<LuaGen>,
    refs: Vec<c_int>,
}

unsafe fn track(L: *mut lua_State, args: &mut BuildArgs, index: c_int) -> c_int {
    let reference = unsafe { reference(L, index) };
    args.refs.push(reference);
    reference
}

/// `ac_plugin_parent`: `_meta.parent` (through a metatable) on every plugin conf.
unsafe fn plugin_parent(L: *mut lua_State, plugins: c_int, parent: c_int) {
    unsafe {
        let plugins = absolute(L, plugins);
        let parent = absolute(L, parent);
        if is_nil(L, plugins) {
            return;
        }
        lua_pushnil(L);
        while lua_next(L, plugins) != 0 {
            let conf = lua_gettop(L);
            if !is_table(L, conf) {
                fail(L, b"plugin configuration must be a table");
            }
            lua_getfield(L, conf, cstr!("_meta"));
            if is_nil(L, -1) {
                pop(L, 1);
                new_table(L);
                lua_pushvalue(L, -1);
                lua_setfield(L, conf, cstr!("_meta"));
            }
            let meta = lua_gettop(L);
            lua_getfield(L, meta, cstr!("parent"));
            if is_nil(L, -1) {
                pop(L, 1);
                new_table(L);
                lua_getfield(L, parent, cstr!("key"));
                lua_setfield(L, -2, cstr!("resource_key"));
                get_global(L, cstr!("tostring"));
                lua_getfield(L, parent, cstr!("modifiedIndex"));
                lua_call(L, 1, 1);
                lua_setfield(L, -2, cstr!("resource_version"));
                if lua_getmetatable(L, meta) != 0 {
                    lua_pushvalue(L, -2);
                    lua_setfield(L, -2, cstr!("parent"));
                    pop(L, 1);
                } else {
                    new_table(L);
                    new_table(L);
                    lua_pushvalue(L, -3);
                    lua_setfield(L, -2, cstr!("parent"));
                    lua_setfield(L, -2, cstr!("__index"));
                    lua_setmetatable(L, meta);
                }
            }
            pop(L, 3);
        }
    }
}

/// `ac_filter_upstream` for literal-IP nodes (domains are rejected).
unsafe fn filter_upstream(L: *mut lua_State, upstream: c_int, parent: c_int, nodes: &[apisix_rs_core::balancer::Node]) {
    unsafe {
        let upstream = absolute(L, upstream);
        if !truthy(L, upstream) {
            return;
        }
        if parent != 0 {
            lua_getfield(L, parent, cstr!("key"));
            lua_setfield(L, upstream, cstr!("resource_key"));
            lua_getfield(L, parent, cstr!("modifiedIndex"));
            if !truthy(L, -1) {
                pop(L, 1);
                lua_getfield(L, upstream, cstr!("modifiedIndex"));
            }
            lua_setfield(L, upstream, cstr!("resource_version"));
            lua_getfield(L, parent, cstr!("value"));
            lua_getfield(L, -1, cstr!("id"));
            lua_remove(L, -2);
        } else {
            lua_pushnil(L);
            lua_setfield(L, upstream, cstr!("resource_key"));
            lua_getfield(L, upstream, cstr!("modifiedIndex"));
            lua_setfield(L, upstream, cstr!("resource_version"));
            lua_getfield(L, upstream, cstr!("id"));
        }
        lua_setfield(L, upstream, cstr!("resource_id"));
        lua_getfield(L, upstream, cstr!("nodes"));
        let original = lua_gettop(L);
        if truthy(L, original) {
            require(L, b"table.isarray");
            lua_pushvalue(L, original);
            lua_call(L, 1, 1);
            let array = truthy(L, -1);
            pop(L, 1);
            if !array {
                lua_createtable(L, nodes.len() as c_int, 0);
                for (index, node) in nodes.iter().enumerate() {
                    lua_createtable(L, 0, 3);
                    push_bytes(L, &node.host);
                    lua_setfield(L, -2, cstr!("host"));
                    lua_pushinteger(L, node.port as isize);
                    lua_setfield(L, -2, cstr!("port"));
                    lua_pushinteger(L, node.weight as isize);
                    lua_setfield(L, -2, cstr!("weight"));
                    lua_rawseti(L, -2, index as c_int + 1);
                }
                lua_setfield(L, upstream, cstr!("nodes"));
            }
        }
        pop(L, 1);
    }
}

/// `ac_resources_prepare` for the supported kinds; returns the wrapper of
/// every item, in configuration order.
unsafe fn build_store(L: *mut lua_State, args: &mut BuildArgs, store: c_int) -> Vec<Vec<c_int>> {
    const KINDS: [(&str, &[u8]); 3] = [("routes", b"routes\0"), ("upstreams", b"upstreams\0"), ("consumers", b"consumers\0")];
    let mut wrappers = Vec::new();
    unsafe {
        for (kind, field) in KINDS {
            new_table(L);
            let object = lua_gettop(L);
            push_bytes(L, kind.as_bytes());
            lua_setfield(L, object, cstr!("key"));
            match json_field(args.root, &format!("{kind}_conf_version")) {
                Some(version) => push_json(L, version, 0),
                None => lua_pushinteger(L, 1),
            }
            lua_setfield(L, object, cstr!("conf_version"));
            new_table(L);
            let values = lua_gettop(L);
            new_table(L);
            let hash = lua_gettop(L);
            let items = json_field(args.root, kind).and_then(Value::as_array).map(Vec::as_slice).unwrap_or_default();
            let mut kind_wrappers = Vec::with_capacity(items.len());
            for (position, item) in items.iter().enumerate() {
                push_json(L, item, 0);
                let value = lua_gettop(L);
                luaL_checktype(L, value, LUA_TTABLE);
                lua_getfield(L, value, cstr!("id"));
                if is_nil(L, -1) {
                    pop(L, 1);
                    lua_getfield(L, value, cstr!("username"));
                    if is_nil(L, -1) {
                        pop(L, 1);
                        push_bytes(L, format!("arr_{}", position + 1).as_bytes());
                    }
                }
                lua_tolstring(L, -1, ptr::null_mut());
                lua_pushvalue(L, -1);
                lua_setfield(L, value, cstr!("id"));
                lua_pushvalue(L, -1);
                lua_rawget(L, hash);
                let slot = if is_nil(L, -1) { lua_objlen(L, values) as c_int + 1 } else { lua_tointeger(L, -1) as c_int };
                let mut replace = true;
                if !is_nil(L, -1) {
                    lua_rawgeti(L, values, slot);
                    lua_getfield(L, -1, cstr!("modifiedIndex"));
                    lua_getfield(L, value, cstr!("modifiedIndex"));
                    replace = is_nil(L, -1) || lua_equal(L, -1, -2) == 0;
                    pop(L, 3);
                }
                pop(L, 1);
                lua_pushinteger(L, slot as isize);
                lua_rawset(L, hash);
                new_table(L);
                let wrapper = lua_gettop(L);
                lua_pushvalue(L, value);
                lua_setfield(L, wrapper, cstr!("value"));
                lua_getfield(L, value, cstr!("modifiedIndex"));
                if is_nil(L, -1) {
                    pop(L, 1);
                    lua_getfield(L, object, cstr!("conf_version"));
                }
                lua_pushvalue(L, -1);
                lua_setfield(L, wrapper, cstr!("orig_modifiedIndex"));
                lua_setfield(L, wrapper, cstr!("modifiedIndex"));
                push_bytes(L, b"/");
                push_bytes(L, kind.as_bytes());
                push_bytes(L, b"/");
                lua_getfield(L, value, cstr!("id"));
                lua_concat(L, 4);
                lua_setfield(L, wrapper, cstr!("key"));
                new_table(L);
                lua_setfield(L, wrapper, cstr!("clean_handlers"));
                lua_getfield(L, value, cstr!("plugins"));
                plugin_parent(L, -1, wrapper);
                pop(L, 1);
                kind_wrappers.push(track(L, args, wrapper));
                if replace {
                    lua_rawseti(L, values, slot);
                } else {
                    pop(L, 1);
                }
                pop(L, 1);
            }
            lua_pushvalue(L, values);
            lua_setfield(L, object, cstr!("values"));
            lua_pushvalue(L, hash);
            lua_setfield(L, object, cstr!("values_hash"));
            lua_pushvalue(L, object);
            lua_setfield(L, store, field.as_ptr() as *const c_char);
            pop(L, 3);
            wrappers.push(kind_wrappers);
        }
    }
    wrappers
}

/// `ac_consumer_prepare`: per auth plugin, the consumer views with their
/// plugin configuration (validated as consumer schema).
unsafe fn build_consumers(L: *mut lua_State, args: &mut BuildArgs, consumers: &[c_int]) -> c_int {
    unsafe {
        new_table(L);
        let all = lua_gettop(L);
        for plugin in loaded() {
            if !plugin.auth {
                continue;
            }
            new_table(L);
            let group = lua_gettop(L);
            new_table(L);
            let nodes = lua_gettop(L);
            for &wrapper_ref in consumers {
                push_ref(L, wrapper_ref);
                let wrapper = lua_gettop(L);
                lua_getfield(L, wrapper, cstr!("value"));
                let value = lua_gettop(L);
                lua_getfield(L, value, cstr!("plugins"));
                let configs = lua_gettop(L);
                if is_table(L, configs) {
                    push_bytes(L, &plugin.name);
                    lua_rawget(L, configs);
                    let config = lua_gettop(L);
                    if is_table(L, config) {
                        push_ref(L, plugin.module_ref);
                        lua_getfield(L, -1, cstr!("check_schema"));
                        if is_function(L, -1) {
                            lua_pushvalue(L, config);
                            lua_pushinteger(L, 1);
                            lua_call(L, 2, 2);
                            if !truthy(L, -2) {
                                let mut message = b"invalid consumer plugin schema: ".to_vec();
                                message.extend_from_slice(to_bytes(L, -1).unwrap_or_default());
                                fail(L, &message);
                            }
                            pop(L, 2);
                        } else {
                            pop(L, 1);
                        }
                        pop(L, 1);
                        // ac_consumer_clone
                        require(L, b"table.clone");
                        lua_pushvalue(L, value);
                        lua_call(L, 1, 1);
                        let result = lua_gettop(L);
                        lua_getfield(L, wrapper, cstr!("modifiedIndex"));
                        lua_setfield(L, result, cstr!("modifiedIndex"));
                        lua_getfield(L, value, cstr!("id"));
                        lua_setfield(L, result, cstr!("consumer_name"));
                        lua_pushvalue(L, config);
                        lua_setfield(L, result, cstr!("auth_conf"));
                        lua_getfield(L, value, cstr!("labels"));
                        if is_table(L, -1) {
                            lua_getfield(L, -1, cstr!("custom_id"));
                            lua_setfield(L, result, cstr!("custom_id"));
                        }
                        pop(L, 1);
                        let next = lua_objlen(L, nodes) as c_int + 1;
                        lua_rawseti(L, nodes, next);
                    }
                    pop(L, 1);
                }
                pop(L, 3);
            }
            if lua_objlen(L, nodes) > 0 {
                lua_pushvalue(L, nodes);
                lua_setfield(L, group, cstr!("nodes"));
                lua_pushinteger(L, lua_objlen(L, nodes) as isize);
                lua_setfield(L, group, cstr!("len"));
                match json_field(args.root, "consumers_conf_version") {
                    Some(version) => push_json(L, version, 0),
                    None => lua_pushinteger(L, 1),
                }
                lua_setfield(L, group, cstr!("conf_version"));
                push_bytes(L, &plugin.name);
                lua_pushvalue(L, group);
                lua_rawset(L, all);
            }
            pop(L, 2);
        }
        let reference = track(L, args, all);
        pop(L, 1);
        reference
    }
}

unsafe extern "C-unwind" fn build_entry(L: *mut lua_State) -> c_int {
    unsafe {
        let args = &mut *(lua_touserdata(L, 1) as *mut BuildArgs);
        new_table(L);
        let store = lua_gettop(L);
        let store_ref = track(L, args, store);
        let wrappers = build_store(L, args, store);
        let (route_wrappers, upstream_wrappers, consumer_wrappers) = (&wrappers[0], &wrappers[1], &wrappers[2]);
        let consumer_plugins_ref = build_consumers(L, args, consumer_wrappers);
        new_table(L);
        let consumer_indexes_ref = track(L, args, -1);
        pop(L, 1);
        let items = json_field(args.root, "routes").and_then(Value::as_array).map(Vec::as_slice).unwrap_or_default();
        let upstream_items = json_field(args.root, "upstreams").and_then(Value::as_array).map(Vec::as_slice).unwrap_or_default();
        let mut routes = Vec::new();
        for (position, item) in items.iter().enumerate() {
            if item.get("status").and_then(Value::as_f64) == Some(0.0) {
                continue;
            }
            let index = routes.len();
            let core_route = &args.generation.core.routes[index];
            push_ref(L, route_wrappers[position]);
            let wrapper = lua_gettop(L);
            lua_getfield(L, wrapper, cstr!("value"));
            let value = lua_gettop(L);
            let value_ref = track(L, args, value);
            let nodes = core_route.upstream.as_ref().map_or(&[][..], |upstream| upstream.nodes.as_slice());
            let upstream_conf_ref = if let Some(reference) = item.get("upstream_id").filter(|value| !value.is_null()) {
                match upstream_items.iter().position(|candidate| candidate.get("id") == Some(reference)) {
                    Some(found) => {
                        push_ref(L, upstream_wrappers[found]);
                        lua_getfield(L, -1, cstr!("value"));
                        filter_upstream(L, -1, lua_gettop(L) - 1, nodes);
                        let reference = track(L, args, -1);
                        pop(L, 2);
                        reference
                    }
                    None => LUA_NOREF,
                }
            } else {
                lua_getfield(L, value, cstr!("upstream"));
                filter_upstream(L, -1, wrapper, nodes);
                let reference = if is_nil(L, -1) { LUA_NOREF } else { track(L, args, -1) };
                pop(L, 1);
                reference
            };
            lua_getfield(L, value, cstr!("plugins"));
            let calls = compile_plan(L, -1, true, &mut args.refs);
            pop(L, 1);
            let mut meta = [LUA_NOREF; 4];
            push_bytes(L, b"route");
            meta[0] = track(L, args, -1);
            pop(L, 1);
            lua_getfield(L, value, cstr!("id"));
            meta[1] = track(L, args, -1);
            pop(L, 1);
            lua_getfield(L, wrapper, cstr!("modifiedIndex"));
            meta[2] = track(L, args, -1);
            pop(L, 1);
            lua_getfield(L, value, cstr!("name"));
            meta[3] = if is_nil(L, -1) { LUA_REFNIL } else { track(L, args, -1) };
            pop(L, 1);
            routes.push(LuaRoute { wrapper_ref: route_wrappers[position], value_ref, upstream_conf_ref, calls, meta: Meta(meta) });
            pop(L, 2);
        }
        if routes.len() != args.generation.core.routes.len() {
            fail(L, b"route compilation mismatch between the Rust core and the Lua view");
        }
        args.out = Some(LuaGen {
            store_ref,
            routes,
            consumer_plugins_ref,
            consumer_indexes_ref,
            merged: RefCell::new(Vec::new()),
            refs: Vec::new(),
        });
        0
    }
}

/// `ac_snapshot_reload`: byte-compared file, transactional rebuild.
unsafe extern "C-unwind" fn reload(L: *mut lua_State) -> c_int {
    unsafe {
        let engine = engine();
        let path = (*engine).path.clone();
        let bytes = match runtime::read_source(&path) {
            Ok(bytes) => bytes,
            Err(error) => fail(L, error.as_bytes()),
        };
        if (*engine).source == bytes {
            return 0;
        }
        let previous = runtime::current();
        let root: Value = match serde_json::from_slice(&bytes) {
            Ok(root) => root,
            Err(error) => fail(L, format!("invalid JSON: {error}").as_bytes()),
        };
        match build_generation(L, &root, previous.as_deref()) {
            Ok(generation) => publish(generation, bytes),
            Err(error) => fail(L, error.as_bytes()),
        }
        0
    }
}

/// Timer callback (`ac_snapshot_tick`): a rejected candidate keeps the
/// current generation and is logged.
unsafe extern "C-unwind" fn tick(L: *mut lua_State) -> c_int {
    unsafe {
        if truthy(L, 1) {
            lua_settop(L, 0);
            lua_pushnil(L);
            return 1;
        }
        lua_settop(L, 0);
        push_fn(L, reload);
        if lua_pcall(L, 0, 0, 0) != 0 {
            let message = to_bytes(L, -1).map(|text| String::from_utf8_lossy(text).into_owned()).unwrap_or_default();
            let log = (*ngx_cycle).log;
            ngx::ngx_log_error!(NGX_LOG_ERR, log, "standalone configuration rejected: {}", message);
        }
        lua_settop(L, 0);
        lua_pushnil(L);
        1
    }
}

// Test-only (`panic-test` feature): `engine.start_phase` panics for
// `GET /__apisix_rs_panic`, to characterize a Rust panic inside the Lua
// adapter (`tests/test_rust_panic.py --mode rust-lua`). Defined at the end
// of the file so the release build keeps the measured code layout.
#[cfg(not(feature = "panic-test"))]
const START_PHASE: lua_CFunction = start_phase;
#[cfg(feature = "panic-test")]
const START_PHASE: lua_CFunction = panic_start_phase;

#[cfg(feature = "panic-test")]
unsafe extern "C-unwind" fn panic_start_phase(L: *mut lua_State) -> c_int {
    if unsafe { http::bytes(&(*request(L)).uri) } == b"/__apisix_rs_panic" {
        panic!("intentional panic from the panic-test build (step 1)");
    }
    unsafe { start_phase(L) }
}
