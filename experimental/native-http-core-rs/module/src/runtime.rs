// SPDX-License-Identifier: Apache-2.0
//! Configuration generations and the standalone file watcher.
//!
//! A request pins the generation it started with (`Rc` clone in its context);
//! publishing a new generation never affects in-flight requests, and the old
//! one is freed when its last request finishes. A rejected candidate keeps the
//! current generation, as in `ac_snapshot_reload`/`ac_snapshot_tick`.

use core::ptr;
use std::cell::RefCell;
use std::rc::Rc;

use apisix_rs_core::config::Generation;
use ngx::ffi::*;
use ngx::http::HttpModuleConfExt;
use serde_json::Value;

use crate::{guard, http, module, MainConf};

const WATCH_INTERVAL_MS: ngx_msec_t = 1000;
const MAX_CONFIG_BYTES: usize = 64 * 1024 * 1024;

/// Socket address of one upstream node, owned by the generation pool.
pub(crate) struct PeerAddr {
    pub(crate) sockaddr: *mut sockaddr,
    pub(crate) socklen: socklen_t,
    pub(crate) name: ngx_str_t,
}

/// A compiled generation plus the NGINX objects derived from it.
pub(crate) struct GenRt {
    pub(crate) core: Generation,
    /// Per route: one address per upstream node, in node order.
    pub(crate) peers: Vec<Vec<PeerAddr>>,
    /// Per route: `ngx_cidr_t` array for `remote_addrs`, or null.
    pub(crate) cidrs: Vec<*mut ngx_array_t>,
    /// Step 1 only: the Lua side (plugin plans, resource and consumer views).
    pub(crate) lua: Option<Box<crate::step1::LuaGen>>,
    pool: *mut ngx_pool_t,
}

impl Drop for GenRt {
    fn drop(&mut self) {
        // Lua references first: they may point at nothing in the pool, but
        // keep the release order of the C snapshot (Lua, then memory).
        self.lua = None;
        if !self.pool.is_null() {
            unsafe { ngx_destroy_pool(self.pool) };
        }
    }
}

impl GenRt {
    unsafe fn build(core: Generation, log: *mut ngx_log_t) -> Result<GenRt, String> {
        let pool = unsafe { ngx_create_pool(16384, log) };
        if pool.is_null() {
            return Err("configuration generation allocation failed".into());
        }
        let mut generation = GenRt { core, peers: Vec::new(), cidrs: Vec::new(), lua: None, pool };
        for route in &generation.core.routes {
            let mut addrs = Vec::new();
            if let Some(upstream) = &route.upstream {
                for node in &upstream.nodes {
                    addrs.push(unsafe { peer_addr(pool, node)? });
                }
            }
            generation.peers.push(addrs);
            let cidrs = if route.remote_addrs.is_empty() {
                ptr::null_mut()
            } else {
                unsafe { cidr_array(pool, &route.remote_addrs)? }
            };
            generation.cidrs.push(cidrs);
        }
        Ok(generation)
    }
}

unsafe fn peer_addr(pool: *mut ngx_pool_t, node: &apisix_rs_core::balancer::Node) -> Result<PeerAddr, String> {
    let mut text = node.ip.to_string().into_bytes();
    let mut addr: ngx_addr_t = unsafe { core::mem::zeroed() };
    if unsafe { ngx_parse_addr(pool, &mut addr, text.as_mut_ptr(), text.len()) } != NGX_OK as ngx_int_t {
        return Err("invalid upstream node address".into());
    }
    unsafe { ngx_inet_set_port(addr.sockaddr, node.port) };
    Ok(PeerAddr {
        sockaddr: addr.sockaddr,
        socklen: addr.socklen,
        // The node name bytes live in the immutable generation for its lifetime.
        name: ngx_str_t { len: node.name.len(), data: node.name.as_ptr() as *mut u8 },
    })
}

unsafe fn cidr_array(pool: *mut ngx_pool_t, addrs: &[Vec<u8>]) -> Result<*mut ngx_array_t, String> {
    let array = unsafe { ngx_array_create(pool, addrs.len(), core::mem::size_of::<ngx_cidr_t>()) };
    if array.is_null() {
        return Err("native route CIDR allocation failed".into());
    }
    for text in addrs {
        let cidr = unsafe { ngx_array_push(array) } as *mut ngx_cidr_t;
        let copy = unsafe { ngx_pnalloc(pool, text.len()) } as *mut u8;
        if cidr.is_null() || copy.is_null() {
            return Err("native route CIDR allocation failed".into());
        }
        unsafe { ptr::copy_nonoverlapping(text.as_ptr(), copy, text.len()) };
        let mut value = ngx_str_t { len: text.len(), data: copy };
        if unsafe { ngx_ptocidr(&mut value, cidr) } == NGX_ERROR as ngx_int_t {
            return Err("invalid native route remote address".into());
        }
    }
    Ok(array)
}

#[derive(Default)]
struct Worker {
    current: Option<Rc<GenRt>>,
    path: String,
    source: Vec<u8>,
    timer: Option<Box<ngx_event_t>>,
}

thread_local! {
    static WORKER: RefCell<Worker> = RefCell::new(Worker::default());
}

/// The generation new requests use.
#[inline]
pub(crate) fn current() -> Option<Rc<GenRt>> {
    WORKER.with(|worker| worker.borrow().current.clone())
}

pub(crate) fn read_source(path: &str) -> Result<Vec<u8>, String> {
    let bytes = std::fs::read(path).map_err(|error| format!("cannot open standalone configuration: {error}"))?;
    if bytes.len() > MAX_CONFIG_BYTES {
        return Err("invalid standalone configuration length".into());
    }
    Ok(bytes)
}

/// Compiles a parsed configuration (reload compatibility checked against
/// `previous`) into a generation with its NGINX objects.
pub(crate) fn compile_value(root: &Value, log: *mut ngx_log_t, previous: Option<&GenRt>) -> Result<GenRt, String> {
    if let Some(previous) = previous {
        previous.core.reload_compatible(root)?;
    }
    let core = Generation::compile(root)?;
    unsafe { GenRt::build(core, log) }
}

fn compile(bytes: &[u8], log: *mut ngx_log_t, previous: Option<&GenRt>) -> Result<GenRt, String> {
    let root: Value = serde_json::from_slice(bytes).map_err(|error| format!("invalid JSON: {error}"))?;
    compile_value(&root, log, previous)
}

/// Publishes the generation new requests use (step 1: from the Lua watcher).
pub(crate) fn publish(generation: Rc<GenRt>) {
    WORKER.with(|worker| worker.borrow_mut().current = Some(generation));
}

/// Compile-only validation during configuration loading (master process).
pub(crate) fn validate(path: &[u8], log: *mut ngx_log_t) -> Result<(), String> {
    let path = std::str::from_utf8(path).map_err(|_| "configuration path is not UTF-8".to_string())?;
    compile(&read_source(path)?, log, None).map(drop)
}

fn reload(log: *mut ngx_log_t) {
    let (path, previous) = WORKER.with(|worker| {
        let worker = worker.borrow();
        (worker.path.clone(), worker.current.clone())
    });
    let result = read_source(&path).and_then(|bytes| {
        let unchanged = WORKER.with(|worker| worker.borrow().source == bytes);
        if unchanged {
            return Ok(None);
        }
        compile(&bytes, log, previous.as_deref()).map(|generation| Some((generation, bytes)))
    });
    match result {
        Ok(None) => {}
        Ok(Some((generation, bytes))) => WORKER.with(|worker| {
            let mut worker = worker.borrow_mut();
            worker.current = Some(Rc::new(generation));
            worker.source = bytes;
        }),
        Err(error) => {
            ngx::ngx_log_error!(NGX_LOG_ERR, log, "standalone configuration rejected: {}", error);
        }
    }
}

unsafe extern "C" fn watcher_tick(event: *mut ngx_event_t) {
    let log = unsafe { (*event).log };
    guard::contain(log, (), "configuration watcher", || reload(log));
    unsafe {
        if ngx_exiting == 0 && ngx_quit == 0 && ngx_terminate == 0 {
            ngx_add_timer(event, WATCH_INTERVAL_MS);
        }
    }
}

pub(crate) unsafe extern "C" fn init_process(cycle: *mut ngx_cycle_t) -> ngx_int_t {
    guard::install_panic_hook();
    let log = unsafe { (*cycle).log };
    guard::contain(log, NGX_ERROR as ngx_int_t, "init_process", || unsafe {
        let Some(main) = (*cycle).http_main_conf_unchecked::<MainConf>(module()) else {
            return NGX_OK as ngx_int_t; // no http block
        };
        let path = http::bytes(&(*main.as_ptr()).config_path);
        if path.is_empty() {
            return NGX_OK as ngx_int_t; // module loaded but not configured
        }
        let Ok(path) = std::str::from_utf8(path) else { return NGX_ERROR as ngx_int_t };
        let initial = read_source(path).and_then(|bytes| compile(&bytes, log, None).map(|g| (g, bytes)));
        let (generation, bytes) = match initial {
            Ok(value) => value,
            Err(error) => {
                ngx::ngx_log_error!(NGX_LOG_EMERG, log, "standalone configuration rejected: {}", error);
                return NGX_ERROR as ngx_int_t;
            }
        };
        let mut timer: Box<ngx_event_t> = Box::new(core::mem::zeroed());
        timer.handler = Some(watcher_tick);
        timer.log = log;
        timer.set_cancelable(1);
        let timer_ptr: *mut ngx_event_t = &mut *timer;
        WORKER.with(|worker| {
            let mut worker = worker.borrow_mut();
            worker.current = Some(Rc::new(generation));
            worker.path = path.to_owned();
            worker.source = bytes;
            worker.timer = Some(timer);
        });
        ngx_add_timer(timer_ptr, WATCH_INTERVAL_MS);
        NGX_OK as ngx_int_t
    })
}

pub(crate) unsafe extern "C" fn exit_process(cycle: *mut ngx_cycle_t) {
    let log = unsafe { (*cycle).log };
    guard::contain(log, (), "exit_process", || {
        WORKER.with(|worker| {
            let mut worker = worker.borrow_mut();
            if let Some(timer) = worker.timer.as_mut() {
                if timer.timer_set() != 0 {
                    unsafe { ngx_del_timer(&mut **timer) };
                }
            }
            worker.current = None;
        })
    });
}
