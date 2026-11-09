// SPDX-License-Identifier: Apache-2.0
//! Panic containment for every NGINX entry point.
//!
//! ngx-rust's handler macros do not catch panics, and a panic unwinding out of
//! an `extern "C"` function aborts the process. Each callback therefore runs
//! inside `contain`, which turns a panic into the callback's failure value
//! (500 for request handlers) and logs it. The release profile keeps
//! `panic = "unwind"` so this works.

use std::any::Any;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Once;

use ngx::ffi::{ngx_cycle, ngx_log_t, NGX_LOG_ALERT, NGX_LOG_ERR};

fn message(payload: &(dyn Any + Send)) -> &str {
    if let Some(text) = payload.downcast_ref::<&'static str>() {
        text
    } else if let Some(text) = payload.downcast_ref::<String>() {
        text.as_str()
    } else {
        "non-string panic payload"
    }
}

/// Runs `body`; on panic logs `what` and returns `fallback`.
#[inline]
pub(crate) fn contain<T>(log: *mut ngx_log_t, fallback: T, what: &str, body: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(payload) => {
            if !log.is_null() {
                ngx::ngx_log_error!(
                    NGX_LOG_ERR,
                    log,
                    "apisix-rs: contained panic in {}: {}",
                    what,
                    message(payload.as_ref())
                );
            }
            fallback
        }
    }
}

/// Routes the panic location to the NGINX error log instead of stderr.
pub(crate) fn install_panic_hook() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        std::panic::set_hook(Box::new(|info| {
            let log = unsafe { if ngx_cycle.is_null() { std::ptr::null_mut() } else { (*ngx_cycle).log } };
            if log.is_null() {
                return;
            }
            let location = info.location().map(|l| format!("{}:{}", l.file(), l.line())).unwrap_or_default();
            ngx::ngx_log_error!(NGX_LOG_ALERT, log, "apisix-rs: panic at {}: {}", location, message(info.payload()));
        }));
    });
}
