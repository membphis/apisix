// SPDX-License-Identifier: Apache-2.0
//! Standalone configuration contract (`conf/native.json`), compiled into an
//! immutable generation.
//!
//! This is the closed-world contract of the C pipeline (`ac_pipeline_compile`,
//! `ac_upstream_validate_native`, `ac_key_auth_prepare/compile`): route-level
//! literal key-auth, static HTTP nodes and the common keepalive settings.
//! Anything the pipeline would not execute is rejected for the whole
//! generation instead of being silently ignored. Unknown fields are rejected
//! too, which is stricter than the C path; see the README for the list.

use serde_json::{Map, Value};
use std::net::IpAddr;

use crate::balancer::{Kind, Node, NodeState, Upstream};
use crate::keyindex::KeyIndex;
use crate::router::{PathSpec, Router};

/// NGINX method bits (`NGX_HTTP_*`); the module asserts these at build time.
pub const METHODS: &[(&str, u32)] = &[
    ("GET", 0x0002),
    ("HEAD", 0x0004),
    ("POST", 0x0008),
    ("PUT", 0x0010),
    ("DELETE", 0x0020),
    ("OPTIONS", 0x0200),
    ("PATCH", 0x4000),
    ("TRACE", 0x8000),
    ("CONNECT", 0x10000),
];

const DEFAULT_TIMEOUT_MS: u64 = 6000;
const KEEPALIVE: (u64, u64, f64) = (320, 1000, 60.0);

#[derive(Debug)]
pub struct Anonymous {
    pub name: Vec<u8>,
    pub consumer: Option<usize>,
}

#[derive(Debug)]
pub struct KeyAuth {
    pub header: Vec<u8>,
    pub query: Vec<u8>,
    /// Complete `WWW-Authenticate` value: `apikey realm="<realm>"`.
    pub challenge: Vec<u8>,
    pub hide: bool,
    pub anonymous: Option<Anonymous>,
}

#[derive(Debug)]
pub struct Consumer {
    pub username: Option<Vec<u8>>,
    pub custom_id: Option<Vec<u8>>,
    /// Always `None` for consumer-level plugin configuration in standalone mode.
    pub credential_id: Option<Vec<u8>>,
}

#[derive(Debug)]
pub struct Route {
    pub id: Vec<u8>,
    /// Lower-cased host patterns (`*.example.com` or exact).
    pub hosts: Vec<Vec<u8>>,
    pub methods: u32,
    pub remote_addrs: Vec<Vec<u8>>,
    pub key_auth: Option<KeyAuth>,
    pub upstream: Option<Upstream>,
    /// Non-zero when the route resolves to an error (missing `upstream_id`).
    pub resolve_status: u16,
}

#[derive(Debug, Clone, Copy)]
pub struct Settings {
    pub host_router: bool,
    pub delete_tail_slash: bool,
    /// `apisix.enable_server_tokens` (default on): `Server: APISIX/<version>`.
    pub server_tokens: bool,
    /// `apisix.show_upstream_status_in_response_header` (default off).
    pub show_upstream_status: bool,
}

#[derive(Debug)]
pub struct Generation {
    pub routes: Vec<Route>,
    pub router: Router,
    pub consumers: Vec<Consumer>,
    pub keys: KeyIndex,
    pub settings: Settings,
    reload_keys: [Value; 3],
}

type Result<T> = std::result::Result<T, String>;

fn object<'a>(value: &'a Value, what: &str) -> Result<&'a Map<String, Value>> {
    value.as_object().ok_or_else(|| format!("{what} must be an object"))
}

fn allow(map: &Map<String, Value>, what: &str, allowed: &[&str]) -> Result<()> {
    for key in map.keys() {
        if !allowed.contains(&key.as_str()) {
            return Err(format!("unsupported field {what}.{key} in the Rust key-auth pipeline"));
        }
    }
    Ok(())
}

fn field<'a>(map: &'a Map<String, Value>, key: &str) -> Option<&'a Value> {
    map.get(key).filter(|value| !value.is_null())
}

fn string(map: &Map<String, Value>, key: &str, what: &str) -> Result<Option<Vec<u8>>> {
    match field(map, key) {
        None => Ok(None),
        Some(Value::String(text)) => Ok(Some(text.as_bytes().to_vec())),
        Some(_) => Err(format!("{what}.{key} must be a string")),
    }
}

fn boolean(map: &Map<String, Value>, key: &str, what: &str) -> Result<Option<bool>> {
    match field(map, key) {
        None => Ok(None),
        Some(Value::Bool(flag)) => Ok(Some(*flag)),
        Some(_) => Err(format!("{what}.{key} must be a boolean")),
    }
}

fn integer(map: &Map<String, Value>, key: &str, what: &str) -> Result<Option<i64>> {
    match field(map, key) {
        None => Ok(None),
        Some(value) => match value.as_i64() {
            Some(number) => Ok(Some(number)),
            None => match value.as_f64() {
                Some(number) if number.fract() == 0.0 && number.abs() < 9.0e15 => Ok(Some(number as i64)),
                _ => Err(format!("{what}.{key} must be an integer")),
            },
        },
    }
}

/// `ac_timeout`: seconds to milliseconds, truncated like `(ngx_msec_t) (s * 1000)`.
fn milliseconds(map: &Map<String, Value>, key: &str, what: &str, fallback: u64) -> Result<u64> {
    match field(map, key) {
        None => Ok(fallback),
        Some(value) => match value.as_f64() {
            Some(seconds) if seconds >= 0.0 && seconds.is_finite() => Ok((seconds * 1000.0) as u64),
            _ => Err(format!("{what}.{key} must be a non-negative number of seconds")),
        },
    }
}

/// `string | [string]` fields such as `uri/uris` and `host/hosts`.
fn strings(map: &Map<String, Value>, single: &str, plural: &str, what: &str) -> Result<Vec<Vec<u8>>> {
    let value = field(map, plural).or_else(|| field(map, single));
    match value {
        None => Ok(Vec::new()),
        Some(Value::String(text)) => Ok(vec![text.as_bytes().to_vec()]),
        Some(Value::Array(items)) => items
            .iter()
            .map(|item| {
                item.as_str()
                    .map(|text| text.as_bytes().to_vec())
                    .ok_or_else(|| format!("{what}.{plural} entries must be strings"))
            })
            .collect(),
        Some(_) => Err(format!("{what}.{plural} must be a string or an array of strings")),
    }
}

/// APISIX `config_util.parse_time_unit` for the units the configuration uses.
fn time_unit(value: &Value, what: &str) -> Result<f64> {
    if let Some(number) = value.as_f64() {
        return Ok(number);
    }
    let text = value.as_str().ok_or_else(|| format!("{what} must be a number or a time string"))?;
    let (digits, scale) = match text.as_bytes().last() {
        Some(b's') if text.ends_with("ms") => (&text[..text.len() - 2], 0.001),
        Some(b's') => (&text[..text.len() - 1], 1.0),
        Some(b'm') => (&text[..text.len() - 1], 60.0),
        Some(b'h') => (&text[..text.len() - 1], 3600.0),
        Some(b'd') => (&text[..text.len() - 1], 86400.0),
        _ => (text, 1.0),
    };
    digits
        .parse::<f64>()
        .map(|number| number * scale)
        .map_err(|_| format!("{what} has an unsupported time value '{text}'"))
}

impl Generation {
    pub fn compile(root: &Value) -> Result<Generation> {
        let root = object(root, "standalone configuration")?;
        allow(
            root,
            "configuration",
            &[
                "routes", "consumers", "global_rules", "plugins", "local_conf", "router", "upstreams",
                "services", "consumer_groups", "plugin_configs", "plugin_metadata", "ssls",
                "stream_routes", "protos", "secrets",
            ],
        )?;
        for kind in [
            "services", "consumer_groups", "plugin_configs", "plugin_metadata", "ssls", "stream_routes",
            "protos", "secrets",
        ] {
            if field(root, kind).and_then(Value::as_array).is_some_and(|items| !items.is_empty()) {
                return Err(format!("Rust key-auth pipeline does not support {kind}"));
            }
        }
        check_plugins(field(root, "plugins"))?;
        let (settings, keepalive) = check_local_conf(field(root, "local_conf"))?;
        let parameter_router = match field(root, "router").map(|value| value.as_str()) {
            None | Some(Some("radixtree_host_uri")) | Some(Some("radixtree_uri")) => false,
            Some(Some("radixtree_uri_with_parameter")) => true,
            Some(other) => return Err(format!("unknown native HTTP router '{}'", other.unwrap_or("non-string"))),
        };
        let settings = Settings {
            host_router: matches!(field(root, "router").and_then(Value::as_str), None | Some("radixtree_host_uri")),
            ..settings
        };
        if let Some(rules) = field(root, "global_rules") {
            if !rules.as_array().is_some_and(Vec::is_empty) {
                return Err("Rust key-auth pipeline requires route-only key-auth; global rules are unsupported".into());
            }
        }

        let (consumers, keys, consumer_ids) = compile_consumers(field(root, "consumers"))?;
        let upstreams = match field(root, "upstreams") {
            None => Vec::new(),
            Some(Value::Array(items)) => items.iter().collect(),
            Some(_) => return Err("upstreams must be an array".into()),
        };

        let mut routes = Vec::new();
        let mut index = Vec::new();
        let items = match field(root, "routes") {
            None => &[][..],
            Some(Value::Array(items)) => items.as_slice(),
            Some(_) => return Err("routes must be an array".into()),
        };
        for item in items {
            let route = object(item, "route")?;
            if integer(route, "status", "route")? == Some(0) {
                continue;
            }
            let (compiled, paths, priority) =
                compile_route(route, parameter_router, &consumer_ids, &upstreams, keepalive)?;
            routes.push(compiled);
            index.push((priority, paths));
        }
        let router = Router::build(index);
        Ok(Generation {
            routes,
            router,
            consumers,
            keys,
            settings,
            reload_keys: reload_keys(root),
        })
    }

    /// Local settings, enabled plugins and the router mode need an NGINX
    /// reload, as in `ac_snapshot_reload`.
    pub fn reload_compatible(&self, next: &Value) -> Result<()> {
        let next = object(next, "standalone configuration")?;
        if reload_keys(next) != self.reload_keys {
            return Err("local settings and enabled plugins require an NGINX reload".into());
        }
        Ok(())
    }
}

fn reload_keys(root: &Map<String, Value>) -> [Value; 3] {
    ["plugins", "local_conf", "router"].map(|key| root.get(key).cloned().unwrap_or(Value::Null))
}

fn check_plugins(plugins: Option<&Value>) -> Result<()> {
    let list = plugins.and_then(Value::as_array).ok_or("plugins must be an array of plugin names")?;
    let mut found = false;
    for plugin in list {
        match plugin.as_str() {
            Some("key-auth") => found = true,
            Some(other) => {
                return Err(format!("Rust key-auth pipeline only supports the key-auth plugin; found {other}"))
            }
            None => return Err("plugins entries must be strings".into()),
        }
    }
    if !found {
        return Err("Rust key-auth pipeline requires key-auth enabled".into());
    }
    Ok(())
}

fn check_local_conf(local: Option<&Value>) -> Result<(Settings, (u64, u64, f64))> {
    let mut settings = Settings { host_router: true, delete_tail_slash: false, server_tokens: true, show_upstream_status: false };
    let mut keepalive = KEEPALIVE;
    let Some(local) = local else { return Ok((settings, keepalive)) };
    let local = object(local, "local_conf")?;
    if let Some(apisix) = field(local, "apisix") {
        let apisix = object(apisix, "local_conf.apisix")?;
        for (key, what) in [
            ("normalize_uri_like_servlet", "servlet URI normalization"),
            ("match_uri_encoded_slash", "encoded-slash matching"),
            ("tracing", "tracing"),
        ] {
            if boolean(apisix, key, "local_conf.apisix")? == Some(true) {
                return Err(format!("Rust key-auth pipeline does not support {what}"));
            }
        }
        if field(apisix, "trusted_addresses").is_some_and(|value| !value.as_array().is_some_and(Vec::is_empty)) {
            return Err("Rust key-auth pipeline does not support trusted addresses".into());
        }
        settings.delete_tail_slash = boolean(apisix, "delete_uri_tail_slash", "local_conf.apisix")?.unwrap_or(false);
        // `ac_http_worker`: only an explicit `false` disables the tokens.
        settings.server_tokens = !matches!(field(apisix, "enable_server_tokens"), Some(Value::Bool(false)));
        // `ac_bool_field`: Lua truthiness.
        settings.show_upstream_status = field(apisix, "show_upstream_status_in_response_header")
            .is_some_and(|value| !matches!(value, Value::Bool(false)));
    }
    let upstream = field(local, "nginx_config")
        .and_then(Value::as_object)
        .and_then(|nginx| field(nginx, "http"))
        .and_then(Value::as_object)
        .and_then(|http| field(http, "upstream"))
        .and_then(Value::as_object);
    if let Some(upstream) = upstream {
        if let Some(size) = integer(upstream, "keepalive", "nginx_config.http.upstream")? {
            keepalive.0 = size.max(0) as u64;
        }
        if let Some(requests) = integer(upstream, "keepalive_requests", "nginx_config.http.upstream")? {
            keepalive.1 = requests.max(0) as u64;
        }
        if let Some(timeout) = field(upstream, "keepalive_timeout") {
            keepalive.2 = time_unit(timeout, "nginx_config.http.upstream.keepalive_timeout")?;
        }
    }
    Ok((settings, keepalive))
}

/// Consumers, their key index, and `(id, consumer)` pairs for anonymous lookup.
type CompiledConsumers = (Vec<Consumer>, KeyIndex, Vec<(Vec<u8>, usize)>);

fn compile_consumers(items: Option<&Value>) -> Result<CompiledConsumers> {
    let items = match items {
        None => &[][..],
        Some(Value::Array(items)) => items.as_slice(),
        Some(_) => return Err("consumers must be an array".into()),
    };
    let mut consumers = Vec::with_capacity(items.len());
    let mut keys = Vec::new();
    let mut ids = Vec::new();
    for item in items {
        let consumer = object(item, "consumer")?;
        if field(consumer, "group_id").is_some() {
            return Err("Rust key-auth pipeline does not support consumer groups".into());
        }
        allow(consumer, "consumer", &["username", "id", "desc", "labels", "plugins", "create_time", "update_time"])?;
        let username = string(consumer, "username", "consumer")?;
        let custom_id = match field(consumer, "labels") {
            None => None,
            Some(labels) => string(object(labels, "consumer.labels")?, "custom_id", "consumer.labels")?,
        };
        let index = consumers.len();
        // Standalone mode keys consumers by `id`, else `username` (config_yaml.lua).
        if let Some(id) = string(consumer, "id", "consumer")?.or_else(|| username.clone()) {
            ids.push((id, index));
        }
        if let Some(plugins) = field(consumer, "plugins") {
            let plugins = object(plugins, "consumer.plugins")?;
            for (name, conf) in plugins {
                if name != "key-auth" {
                    return Err("Rust key-auth pipeline: consumer contains an unsupported plugin".into());
                }
                let conf = object(conf, "consumer key-auth")?;
                allow(conf, "consumer.plugins.key-auth", &["key"])?;
                let key = string(conf, "key", "consumer key-auth")?
                    .ok_or("consumer key-auth requires a key")?;
                if key.starts_with(b"$secret://") || key.len() >= 7 && key[..7].eq_ignore_ascii_case(b"$ENV://") {
                    return Err("Rust key-auth pipeline requires literal consumer keys; secret providers are unsupported".into());
                }
                keys.push((key, index));
            }
        }
        consumers.push(Consumer { username, custom_id, credential_id: None });
    }
    Ok((consumers, KeyIndex::build(keys), ids))
}

fn compile_route(
    route: &Map<String, Value>,
    parameter_router: bool,
    consumer_ids: &[(Vec<u8>, usize)],
    upstreams: &[&Value],
    keepalive: (u64, u64, f64),
) -> Result<(Route, Vec<PathSpec>, i64)> {
    if field(route, "filter_func").is_some() || field(route, "vars").is_some() {
        return Err("Rust key-auth pipeline does not support Lua route predicates".into());
    }
    if field(route, "script").is_some() {
        return Err("Rust key-auth pipeline does not support Lua route scripts".into());
    }
    for (key, what) in [("service_id", "services"), ("plugin_config_id", "plugin configs")] {
        if field(route, key).is_some() {
            return Err(format!("Rust key-auth pipeline does not support {what}"));
        }
    }
    if boolean(route, "enable_websocket", "route")? == Some(true) {
        return Err("Rust key-auth pipeline does not support WebSocket proxying".into());
    }
    allow(
        route,
        "route",
        &[
            "id", "name", "desc", "labels", "uri", "uris", "host", "hosts", "methods", "remote_addr",
            "remote_addrs", "priority", "plugins", "upstream", "upstream_id", "status", "timeout",
            "enable_websocket", "create_time", "update_time",
        ],
    )?;
    let id = match field(route, "id") {
        None => Vec::new(),
        Some(Value::String(text)) => text.as_bytes().to_vec(),
        Some(Value::Number(number)) => number.to_string().into_bytes(),
        Some(_) => return Err("route.id must be a string or a number".into()),
    };
    let sources = strings(route, "uri", "uris", "route")?;
    if sources.is_empty() {
        return Err("native route requires at least one URI".into());
    }
    let paths = sources
        .iter()
        .map(|source| PathSpec::compile(source, parameter_router))
        .collect::<Result<Vec<_>>>()?;
    let hosts = strings(route, "host", "hosts", "route")?
        .into_iter()
        .map(|host| host.to_ascii_lowercase())
        .collect();
    let mut methods = 0;
    if let Some(list) = field(route, "methods") {
        for method in list.as_array().ok_or("route.methods must be an array")? {
            let name = method.as_str().ok_or("route.methods entries must be strings")?;
            let bit = METHODS
                .iter()
                .find(|(known, _)| *known == name)
                .map(|(_, bit)| *bit)
                .ok_or_else(|| format!("invalid route method '{name}'"))?;
            methods |= bit;
        }
    }
    let remote_addrs = strings(route, "remote_addr", "remote_addrs", "route")?;
    let priority = integer(route, "priority", "route")?.unwrap_or(0);

    let mut key_auth = None;
    if let Some(plugins) = field(route, "plugins") {
        for (name, conf) in object(plugins, "route.plugins")? {
            if name != "key-auth" {
                return Err(format!("Rust key-auth pipeline only supports route-level key-auth; found {name}"));
            }
            key_auth = compile_key_auth(object(conf, "route key-auth")?, consumer_ids)?;
        }
    }

    let route_timeout = match field(route, "timeout") {
        None => None,
        Some(value) => Some(object(value, "route.timeout")?),
    };
    let mut resolve_status = 0;
    let upstream = if let Some(reference) = field(route, "upstream_id") {
        let found = upstreams.iter().find(|item| item.get("id") == Some(reference));
        match found {
            Some(conf) => Some(compile_upstream(object(conf, "upstream")?, route_timeout, keepalive)?),
            None => {
                resolve_status = 503;
                None
            }
        }
    } else {
        match field(route, "upstream") {
            Some(conf) => Some(compile_upstream(object(conf, "route.upstream")?, route_timeout, keepalive)?),
            None => None,
        }
    };
    Ok((Route { id, hosts, methods, remote_addrs, key_auth, upstream, resolve_status }, paths, priority))
}

fn compile_key_auth(conf: &Map<String, Value>, consumer_ids: &[(Vec<u8>, usize)]) -> Result<Option<KeyAuth>> {
    allow(conf, "route.plugins.key-auth", &["header", "query", "realm", "hide_credentials", "anonymous_consumer", "_meta"])?;
    if let Some(meta) = field(conf, "_meta") {
        let meta = object(meta, "key-auth._meta")?;
        if field(meta, "error_response").is_some() {
            return Err("native key-auth error_response is unsupported".into());
        }
        if field(meta, "filter").is_some() || field(meta, "pre_function").is_some() {
            return Err("native key-auth experiment requires route-only key-auth without Lua metadata callbacks".into());
        }
        allow(meta, "key-auth._meta", &["disable", "priority"])?;
        if boolean(meta, "disable", "key-auth._meta")? == Some(true) {
            return Ok(None);
        }
    }
    let realm = string(conf, "realm", "key-auth")?.unwrap_or_else(|| b"key".to_vec());
    let mut challenge = b"apikey realm=\"".to_vec();
    challenge.extend_from_slice(&realm);
    challenge.push(b'"');
    let anonymous = string(conf, "anonymous_consumer", "key-auth")?.map(|name| {
        let consumer = consumer_ids.iter().find(|(id, _)| *id == name).map(|(_, index)| *index);
        Anonymous { name, consumer }
    });
    Ok(Some(KeyAuth {
        header: string(conf, "header", "key-auth")?.unwrap_or_else(|| b"apikey".to_vec()),
        query: string(conf, "query", "key-auth")?.unwrap_or_else(|| b"apikey".to_vec()),
        challenge,
        hide: boolean(conf, "hide_credentials", "key-auth")?.unwrap_or(false),
        anonymous,
    }))
}

const NODES_ERROR: &str = "Rust pipeline requires static HTTP nodes, native selection and pass/rewrite host";

fn compile_upstream(
    conf: &Map<String, Value>,
    route_timeout: Option<&Map<String, Value>>,
    keepalive: (u64, u64, f64),
) -> Result<Upstream> {
    allow(
        conf,
        "upstream",
        &[
            "type", "nodes", "scheme", "pass_host", "upstream_host", "retries", "retry_timeout", "timeout",
            "keepalive_pool", "name", "desc", "id", "labels", "create_time", "update_time",
        ],
    )?;
    let kind = match string(conf, "type", "upstream")?.as_deref() {
        None | Some(b"roundrobin") => Kind::RoundRobin,
        Some(b"least_conn") => Kind::LeastConn,
        Some(b"ewma") => Kind::Ewma,
        Some(b"chash") => return Err(NODES_ERROR.into()),
        Some(_) => return Err("unknown upstream balancer type".into()),
    };
    if !matches!(string(conf, "scheme", "upstream")?.as_deref(), None | Some(b"http")) {
        return Err(NODES_ERROR.into());
    }
    let rewrite_host = match string(conf, "pass_host", "upstream")?.as_deref() {
        None | Some(b"pass") => None,
        Some(b"rewrite") => Some(string(conf, "upstream_host", "upstream")?.unwrap_or_default()),
        Some(_) => return Err(NODES_ERROR.into()),
    };
    let mut pool = keepalive;
    if let Some(options) = field(conf, "keepalive_pool") {
        let options = object(options, "upstream.keepalive_pool")?;
        if let Some(size) = integer(options, "size", "keepalive_pool")? {
            pool.0 = size.max(0) as u64;
        }
        if let Some(requests) = integer(options, "requests", "keepalive_pool")? {
            pool.1 = requests.max(0) as u64;
        }
        if let Some(idle) = field(options, "idle_timeout").and_then(Value::as_f64) {
            pool.2 = idle;
        }
    }
    if pool != KEEPALIVE {
        return Err("Rust pipeline requires the configured common 320/1000/60s keepalive settings".into());
    }
    let retries = match integer(conf, "retries", "upstream")? {
        None => None,
        Some(count) => Some(u32::try_from(count).map_err(|_| "upstream.retries must be a non-negative integer")?),
    };
    let retry_timeout_ms = milliseconds(conf, "retry_timeout", "upstream", 0)?;
    let timeout = match route_timeout {
        Some(table) => Some(table),
        None => match field(conf, "timeout") {
            Some(value) => Some(object(value, "upstream.timeout")?),
            None => None,
        },
    };
    let (connect, send, read) = match timeout {
        Some(table) => (
            milliseconds(table, "connect", "timeout", DEFAULT_TIMEOUT_MS)?,
            milliseconds(table, "send", "timeout", DEFAULT_TIMEOUT_MS)?,
            milliseconds(table, "read", "timeout", DEFAULT_TIMEOUT_MS)?,
        ),
        None => (DEFAULT_TIMEOUT_MS, DEFAULT_TIMEOUT_MS, DEFAULT_TIMEOUT_MS),
    };
    if connect == 0 || send == 0 || read == 0 {
        return Err("native upstream timeouts must be positive".into());
    }
    let mut nodes = Vec::new();
    match field(conf, "nodes") {
        None => {}
        Some(Value::Array(items)) => {
            for item in items {
                let node = object(item, "upstream node")?;
                allow(node, "upstream.nodes[]", &["host", "port", "weight", "priority", "metadata"])?;
                let host = string(node, "host", "upstream node")?.ok_or("upstream node requires a host")?;
                let port = match integer(node, "port", "upstream node")? {
                    None => 80,
                    Some(port) => u16::try_from(port).map_err(|_| "invalid upstream node port")?,
                };
                let weight = integer(node, "weight", "upstream node")?.unwrap_or(1);
                let priority = integer(node, "priority", "upstream node")?.unwrap_or(0);
                nodes.push(node_from(host, port, weight, priority)?);
            }
        }
        Some(Value::Object(map)) => {
            for (endpoint, weight) in map {
                let (host, port) = split_endpoint(endpoint)?;
                let weight = weight.as_i64().ok_or("upstream node weight must be an integer")?;
                nodes.push(node_from(host, port, weight, 0)?);
            }
        }
        Some(_) => return Err("upstream.nodes must be an array or an object".into()),
    }
    Ok(Upstream {
        kind,
        nodes,
        rewrite_host,
        retries,
        retry_timeout_ms,
        connect_timeout_ms: connect,
        send_timeout_ms: send,
        read_timeout_ms: read,
        max_fails: 0,
        fail_timeout_ms: 10000,
    })
}

/// `ngx_parse_url` for `host:port` keys; IPv6 hosts keep their brackets.
fn split_endpoint(endpoint: &str) -> Result<(Vec<u8>, u16)> {
    let invalid = || "invalid upstream node key".to_string();
    if endpoint.starts_with('[') {
        let close = endpoint.find(']').ok_or_else(invalid)?;
        let host = endpoint.as_bytes()[..=close].to_vec();
        let rest = &endpoint[close + 1..];
        let port = match rest.strip_prefix(':') {
            None if rest.is_empty() => 80,
            Some(port) => port.parse().map_err(|_| invalid())?,
            None => return Err(invalid()),
        };
        return Ok((host, port));
    }
    match endpoint.rsplit_once(':') {
        Some((host, port)) => Ok((host.as_bytes().to_vec(), port.parse().map_err(|_| invalid())?)),
        None => Ok((endpoint.as_bytes().to_vec(), 80)),
    }
}

/// `ac_add_node` for static IP literals; domains are rejected in the pipeline.
fn node_from(host: Vec<u8>, port: u16, weight: i64, priority: i64) -> Result<Node> {
    if !(0..=65535).contains(&weight) {
        return Err("invalid upstream node weight".into());
    }
    let text = std::str::from_utf8(&host).map_err(|_| "invalid upstream node address")?;
    let literal = text.strip_prefix('[').and_then(|inner| inner.strip_suffix(']')).unwrap_or(text);
    let ip: IpAddr = literal.parse().map_err(|_| NODES_ERROR.to_string())?;
    let name = if text.contains(':') && !text.starts_with('[') {
        format!("[{text}]:{port}")
    } else {
        format!("{text}:{port}")
    };
    Ok(Node {
        host,
        port,
        weight: weight as u32,
        priority,
        ip,
        name: name.into_bytes(),
        state: NodeState::default(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn base() -> Value {
        json!({
            "plugins": ["key-auth"],
            "global_rules": [],
            "local_conf": {"apisix": {"delete_uri_tail_slash": false, "trusted_addresses": null, "tracing": false},
                           "nginx_config": {"http": {"upstream": {"keepalive": 320, "keepalive_requests": 1000, "keepalive_timeout": "60s"}}}},
            "consumers": [
                {"username": "demo-user", "labels": {"custom_id": "custom-user"}, "plugins": {"key-auth": {"key": "demo-key"}}},
                {"username": "guest", "plugins": {}},
                {"username": "dup", "plugins": {"key-auth": {"key": "demo-key"}}}
            ],
            "routes": [
                {"id": "auth", "uri": "/auth/*", "plugins": {"key-auth": {"hide_credentials": true}},
                 "upstream": {"type": "roundrobin", "nodes": {"127.0.0.1:1980": 1}, "retries": 0,
                              "timeout": {"connect": 0.1, "read": 0.1, "send": 0.1}}},
                {"id": "anon", "uri": "/anon/*", "plugins": {"key-auth": {"anonymous_consumer": "guest"}}},
                {"id": "off", "uri": "/off/*", "plugins": {"key-auth": {"_meta": {"disable": true}}}},
                {"id": "gone", "uri": "/gone/*", "status": 0}
            ]
        })
    }

    #[test]
    fn compiles_the_benchmark_shape() {
        let generation = Generation::compile(&base()).unwrap();
        assert_eq!(generation.routes.len(), 3);
        let auth = generation.routes[0].key_auth.as_ref().unwrap();
        assert_eq!(auth.header, b"apikey");
        assert_eq!(auth.challenge, b"apikey realm=\"key\"");
        assert!(auth.hide);
        let upstream = generation.routes[0].upstream.as_ref().unwrap();
        assert_eq!(upstream.nodes[0].name, b"127.0.0.1:1980");
        assert_eq!((upstream.connect_timeout_ms, upstream.retries), (100, Some(0)));
        // Latest duplicate key wins.
        assert_eq!(generation.keys.find(b"demo-key"), Some(2));
        assert_eq!(generation.consumers[0].custom_id.as_deref(), Some(&b"custom-user"[..]));
        let anon = generation.routes[1].key_auth.as_ref().unwrap().anonymous.as_ref().unwrap();
        assert_eq!(anon.consumer, Some(1));
        assert!(generation.routes[2].key_auth.is_none());
        assert!(generation.settings.host_router);
    }

    fn rejected(edit: impl FnOnce(&mut Value)) -> String {
        let mut config = base();
        edit(&mut config);
        Generation::compile(&config).unwrap_err()
    }

    #[test]
    fn rejects_everything_outside_the_contract() {
        assert!(rejected(|c| c["routes"][0]["filter_func"] = json!("function() return true end")).contains("Lua route predicates"));
        assert!(rejected(|c| c["routes"][0]["plugins"]["limit-count"] = json!({})).contains("route-level key-auth"));
        assert!(rejected(|c| c["plugins"] = json!(["key-auth", "prometheus"])).contains("only supports the key-auth"));
        assert!(rejected(|c| c["routes"][0]["upstream"]["nodes"] = json!({"example.com:80": 1})).contains("static HTTP nodes"));
        assert!(rejected(|c| c["routes"][0]["upstream"]["type"] = json!("chash")).contains("static HTTP nodes"));
        assert!(rejected(|c| c["routes"][0]["upstream"]["keepalive_pool"] = json!({"size": 1})).contains("keepalive"));
        assert!(rejected(|c| c["consumers"][0]["plugins"]["key-auth"]["key"] = json!("$secret://vault/x")).contains("literal"));
        assert!(rejected(|c| c["routes"][0]["plugins"]["key-auth"]["_meta"] = json!({"error_response": {}})).contains("error_response"));
        assert!(rejected(|c| c["global_rules"] = json!([{"id": "g", "plugins": {"key-auth": {}}}])).contains("global rules"));
        assert!(rejected(|c| c["routes"][0]["uri"] = json!("/a/*/b")).contains("regular expression"));
        assert!(rejected(|c| c["routes"][0]["upstream"]["checks"] = json!({})).contains("unsupported field"));
        assert!(rejected(|c| c["local_conf"]["apisix"]["trusted_addresses"] = json!(["10.0.0.0/8"])).contains("trusted"));
    }

    #[test]
    fn reload_requires_same_local_settings() {
        let generation = Generation::compile(&base()).unwrap();
        let mut next = base();
        next["routes"][0]["upstream"]["timeout"]["read"] = json!(3);
        assert!(generation.reload_compatible(&next).is_ok());
        next["plugins"] = json!(["key-auth", "prometheus"]);
        assert!(generation.reload_compatible(&next).is_err());
    }

    #[test]
    fn missing_upstream_reference_resolves_to_503() {
        let mut config = base();
        config["routes"][0].as_object_mut().unwrap().remove("upstream");
        config["routes"][0]["upstream_id"] = json!("missing");
        let generation = Generation::compile(&config).unwrap();
        assert_eq!(generation.routes[0].resolve_status, 503);
    }

    #[test]
    fn endpoints_and_array_nodes() {
        assert_eq!(split_endpoint("127.0.0.1:1980").unwrap(), (b"127.0.0.1".to_vec(), 1980));
        assert_eq!(split_endpoint("[::1]:80").unwrap(), (b"[::1]".to_vec(), 80));
        let node = node_from(b"::1".to_vec(), 81, 1, 0).unwrap();
        assert_eq!(node.name, b"[::1]:81");
        assert!(node_from(b"127.0.0.1".to_vec(), 1, 70000, 0).is_err());
    }
}
