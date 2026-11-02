-- SPDX-License-Identifier: Apache-2.0
local tab = require("apisix.core.table")
local str = require("apisix.core.string")
local json = require("apisix.core.json")
local runtime = require("apisix.native.runtime")
local function tuple(...) return {n = select("#", ...), ...} end
local passthrough = runtime.bind(function(...) return nil, ... end)
local values = tuple(passthrough("first", nil, false, nil))
assert(values.n == 4 and values[1] == "first" and values[2] == nil and values[3] == false and values[4] == nil)
local resumed = runtime.bind(function()
    return function(ok, ...)
        assert(ok and select("#", ...) == 3)
        return nil, ...
    end, function(...)
        assert(select("#", ...) == 3 and select(1, ...) == "left" and select(3, ...) == "right")
        return nil, false, nil
    end, {n = 3, [1] = "left", [3] = "right"}
end)
values = tuple(resumed())
assert(values.n == 3 and values[1] == nil and values[2] == false and values[3] == nil)
local a = {a = 1}
a.self = a
a.alias = a
local copy = tab.deepcopy(a)
assert(copy ~= a and copy.self == copy and copy.alias == copy)
local conf = {value = {plugins = {p = {x = true}}, plugins2 = {q = {x = true}}}}
local c = tab.deepcopy(conf, {shallow_prefix = "self.value.plugins"})
assert(c.value.plugins ~= conf.value.plugins and c.value.plugins.p == conf.value.plugins.p)
assert(c.value.plugins2.q ~= conf.value.plugins2.q)
local mt = {marker = 1}
assert(getmetatable(tab.deepcopy(setmetatable({}, mt))) == mt)
local t = {}
assert(tab.insert_tail(t, "x", "y") == 2 and t[2] == "y")
tab.set(t, 3, 4)
assert(t[1] == 3 and t[2] == 4 and tab.array_find(t, 4) == 2)
assert(tab.try_read_attr(conf, "value", "plugins", "p", "x") == true)
assert(tab.try_read_attr(1, "x") == nil)
assert(tab.deep_eq({a = {b = 1}}, {a = {b = 1}}))
assert(not tab.deep_eq({a = 1}, {a = 2}))
assert(tab.set_eq({a = 1}, {a = 2}))
assert(tab.deep_eq(tab.pick({a = 1, b = 2}, {a = false}), {a = 1}))
assert(tab.merge({a = {b = 1}}, {a = {c = 2}}).a.b == 1)
assert(tab.merge({a = 1}, {a = ngx.null}).a == nil)
assert(str.has_prefix("abc", "ab") and not str.has_prefix("a", "abc"))
assert(str.has_suffix("abc", "bc") and not str.has_suffix("a", "abc"))
local first, last = str.find("a.b", ".")
assert(first == 2 and last == 2)
assert(str.rfind_char("abba", "b") == 3)
assert(str.decode_args("a=x%20y&a=z").a[1] == "x y")
assert(str.compress_script("  a,  b\n  c") == "a,b\nc")
assert(json.encode({x = 1}) == '{"x":1}')
assert(json.decode('{"x":null}', {null_as_nil = true}).x == nil)
assert(json.decode("null", {null_as_nil = true}) == nil)
assert(json.decode("null") == ngx.null)
assert(json.encode({x = function() end}, true):find("function:", 1, true))
assert(tostring(json.delay_encode({x = 1})) == '{"x":1}')
local log = require("apisix.core.log")
log.new("prefix").warn("sdk probe")
log.warn(log.delay_exec(function(x) return x end, "delayed"))
ngx.shared.probes:set("sdk-checks", 32)
local schema = require("apisix.core.schema")
local defs = require("apisix.schema_def")
assert(schema.TYPE_CONSUMER == 1 and schema.TYPE_METADATA == 2)
assert(schema.route == defs.route)
assert(defs.route.properties.upstream == defs.upstream)
local s = {type = "object", properties = {value = {type = "integer", default = 7}},
           additionalProperties = false}
local d = {}
assert(schema.check(s, d) and d.value == 7)
assert(not schema.check(s, {value = "no"}))
assert(not schema.check(s, {extra = true}))
assert(schema.check(defs.get_realm_schema("key"), "valid realm"))
assert(not schema.check(defs.get_realm_schema("key"), 'bad"realm'))
local routes, version = require("apisix.router").http_routes()
assert(#routes >= 20 and version == 1)
local config = require("apisix.core.config")
local reader = config.new("/routes")
assert(reader:get("exact") == routes[1])
assert(reader:get("exact").value.plugins.route._meta.parent.resource_key == "/routes/exact")
assert(rawget(reader:get("exact").value.plugins.route._meta, "parent") == nil)
assert(config.local_conf().deployment.config_provider == "json")
require("apisix.core.ctx").register_var("native_yield", function(ctx)
    ngx.sleep(0.001)
    ctx.getter_calls = (ctx.getter_calls or 0) + 1
    return "yielded"
end)
require("apisix.core.ctx").register_var("native_live", function(ctx)
    ctx.live_calls = (ctx.live_calls or 0) + 1
    return ctx.live_calls
end, {no_cacheable = true})
require("apisix.core.ctx").register_var("native_false", function(ctx)
    ctx.false_getter_calls = (ctx.false_getter_calls or 0) + 1
    return false
end)
local cache = require("apisix.core.lrucache")
local local_cache = cache.new({count = 8, ttl = 60})
local calls = 0
local function create(value) calls = calls + 1; return {value = value} end
local one = local_cache("one", 1, create, 11)
assert(local_cache("one", 1, create, 12) == one and one.value == 11 and calls == 1)
assert(local_cache("one", 2, create, 12).value == 12 and calls == 2)
local false_cache = cache.new()
assert(false_cache("false", 1, function() return false end) == false)
assert(false_cache("false", 1, function() error("cache miss") end) == false)
local negative = cache.new({neg_ttl = 1, neg_count = 8})
local _, err = negative("missing", 1, function() return nil, "missing value" end)
assert(err == "missing value")
local _, err2 = negative("missing", 1, function() error("negative miss") end)
assert(err2 == "missing value")
assert(cache.plugin_ctx_id({conf_type = "route", conf_id = 7, conf_version = 3}, "x") == "route#7#x#3")
package.loaded["native_cache_probe"] = cache.new({serial_creating = true})
local utils = require("apisix.core.utils")
local info_ctx = {var = {}}
utils.set_var_rate_limiting_info(info_ctx, 'key"\\\n', 10, "7", 3, {
    window_type = "fixed", window_size = 60, decision = "allowed", cost = 1,
    now = 117.2, window_end = 120.125, count = 3, created = false,
})
assert(tab.deep_eq(json.decode(info_ctx.var.rate_limiting_info), {
    rate_limiting_key = 'key"\\\n', rate_limiting_limit = 10,
    rate_limiting_remaining = 7, rate_limiting_reset = 3,
    window_type = "fixed", window_size_ms = 60000, decision = "allowed", cost = 1,
    evaluated_at_ms = 117200, current_window = {
        start_ms = 60125, end_ms = 120125, count = 3, created = false,
    },
}))
assert(utils.parse_addr("[::1]:123") == "[::1]")
local host, port = utils.parse_addr("example.com:80")
assert(host == "example.com" and port == 80)
assert(utils.parse_addr("2001:db8::1") == "2001:db8::1")
assert(utils.escape_uri_control_chars("a\r\nb?x=1&y=2") == "a%0D%0Ab?x=1&y=2")
assert(utils.uri_safe_encode("x y") == "x%20y")
local resolved, rerr, count = utils.resolve_var("a=$x,b=${ y ?? default },c=$z", {x = 1})
assert(resolved == "a=1,b=default,c=" and rerr == nil and count == 2, tostring(resolved))
assert(utils.resolve_var_with_captures("/$1/${2}", {"one", "two"}) == "/one/two")
assert(require("apisix.core.config_util").parse_time_unit("1h 30m") == 5400)
assert(require("apisix.core.math").gcd(30, 18) == 6)
local sys = require("apisix.core.os")
local env = require("apisix.core.env")
assert(sys.setenv("APISIX_NATIVE_TEST", "first"))
assert(sys.setenv("APISIX_NATIVE_TEST_FILE", "second"))
env.init()
assert(env.get("APISIX_NATIVE_TEST") == "first" and env.get("APISIX_NATIVE_TEST_FILE") == "second")
assert(env.fetch_by_uri("$env://APISIX_NATIVE_TEST_FILE") == "second")
assert(sys.setenv("APISIX_NATIVE_TEST_JSON", '{"sub":"value"}'))
assert(env.fetch_by_uri("$ENV://APISIX_NATIVE_TEST_JSON/sub") == "value")
local ip = require("apisix.core.ip")
assert(ip.validate_cidr_or_ip("127.0.0.1/8"))
assert(not ip.validate_cidr_or_ip("127.0.0.1/33"))
assert(ip.create_ip_matcher({"127.0.0.0/8"}):match("127.0.0.1"))
local crypto = require("apisix.core.data_encryption")
local first = crypto.init_iv_tbl({"1234567890123456"})
local second = crypto.init_iv_tbl({"12345678901234567890123456789012"})
local ciphertext = assert(crypto.aes_cbc_encrypt(first, "native encryption"))
assert(crypto.aes_cbc_decrypt(first, ciphertext) == "native encryption")
assert(crypto.aes_cbc_decrypt({second[1], first[1]}, ciphertext) == "native encryption")
assert(crypto.aes_cbc_decrypt(second, crypto.aes_cbc_encrypt(second, "AES-256")) == "AES-256")
assert(crypto.encrypt("unchanged") == "unchanged" and crypto.decrypt("unchanged") == "unchanged")
local resource = require("apisix.resource")
assert(resource.fetch_latest_conf("/routes/exact#plugins.route") == routes[1])
assert(resource.fetch_latest_conf("/consumer_groups/cg").value.id == "cg")
assert(resource.fetch_latest_conf("/plugin_configs/p").value.id == "p")
local up_util = require("apisix.utils.upstream")
assert(up_util.version(nil, 7) == nil and up_util.version(10) == "10")
assert(up_util.version("10", 0) == "100" and up_util.version(10, 3) == "103")
local metadata = {key = "identity"}
local first_nodes = {{host = "b", port = 80, weight = 2}, {host = "a", metadata = metadata}}
local new_nodes = {{host = "a", metadata = metadata}, {host = "b", port = 80, weight = 2}}
assert(up_util.compare_upstream_node({nodes = first_nodes}, new_nodes))
assert(first_nodes[1].host == "a" and new_nodes[1].host == "a")
assert(not up_util.compare_upstream_node({nodes = first_nodes}, {
    {host = "a", metadata = {key = "identity"}}, {host = "b", port = 80, weight = 2}}))
assert(up_util.compare_upstream_node({nodes = {{host = "filled"}}, original_nodes = {{host = "bare"}}},
                                    {{host = "bare"}}))
assert(not up_util.compare_upstream_node({nodes = {}, original_nodes = {{host = "bare"}}}, {{host = "bare"}}))
local wrapper = tab.deepcopy(reader:get("exact"))
local pcfg = require("apisix.plugin_config")
local merged = pcfg.merge(wrapper, resource.fetch_latest_conf("/plugin_configs/p"))
assert(merged == wrapper and merged.value.plugins.tail.tag == "plugin-config")
assert(merged.value.plugins.route.yield and merged.prev_plugin_config_ver == 1)
return true
