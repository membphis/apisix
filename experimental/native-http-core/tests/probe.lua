-- SPDX-License-Identifier: Apache-2.0
local engine = require("apisix.native.engine")
local M = {}

function M.new(name, priority, prefer)
    local plugin = {name = name, priority = priority, run_policy = prefer}
    function plugin.check_schema(conf)
        return type(conf) == "table", "expected table"
    end
    function plugin.init()
        ngx.shared.probes:incr("init:" .. name, 1, 0)
    end
    function plugin.init_worker()
        ngx.shared.probes:incr("worker:" .. name, 1, 0)
        if name == "route" then
            local event = require("apisix.core.event")
            event.register("native-timer", function()
                ngx.sleep(0.001)
                ngx.shared.probes:incr("timer-fired", 1, 0)
                return "discarded"
            end)
            local timer = assert(require("apisix.core.timer").new("native-timer", function()
                assert(event.push("native-timer") == nil)
            end, {each_ttl = 0, sleep_succ = 0.005, check_interval = 10}))
            assert(ngx.timer.at(0.05, function()
                assert(not timer.running)
                ngx.shared.probes:incr("timer-finished", 1, 0)
            end))
        end
    end
    local function record(conf, ctx, phase)
        assert(ctx == engine.context())
        assert(ctx == ngx.ctx.api_ctx and ctx.ngx_ctx == ngx.ctx)
        if name == "global" and phase == "log" then
            -- Differential checks include successful, rejected, failed and
            -- retried upstreams. Numeric types must match the original SDK.
            local fetch = require("resty.ngxvar").fetch
            for _, key in ipairs({"status", "request_length", "bytes_sent", "upstream_response_time",
                                   "upstream_header_time", "upstream_connect_time", "host", "scheme",
                                   "remote_addr", "request_method", "HOST"}) do
                local actual, expected = ctx.var[key], fetch(key)
                assert(actual == expected, key .. ": " .. tostring(actual) .. " ~= " .. tostring(expected))
            end
        end
        ctx.trace = ctx.trace or {}
        ctx.trace[#ctx.trace + 1] = (conf.tag or name) .. ":" .. phase
        if phase == "rewrite" and conf.yield then
            ngx.sleep(0.001)
            local sock = ngx.socket.tcp()
            assert(sock:connect("127.0.0.1", 18881))
            assert(sock:send("GET / HTTP/1.0\r\nHost: upstream\r\n\r\n"))
            assert(sock:receive("*a"))
            sock:close()
            assert(ctx == engine.context())
        end
        if phase == "rewrite" and conf.hold and conf.hold > 0 then
            ngx.shared.probes:set("trace:snapshot-ready", true)
            ngx.sleep(conf.hold)
        end
        if phase == "rewrite" and conf.fail then error("probe failure") end
        if phase == "rewrite" and conf.rate_observer then
            local detail = ngx.var.arg_shape ~= "basic" and {
                window_type = "fixed", window_size = 60, decision = "allowed",
                now = 117.2, window_end = 120.125, count = 3, created = false,
            } or nil
            require("apisix.core.utils").set_var_rate_limiting_info(ctx, "observer", 10, 7, 3, detail)
            if detail then detail.count = 999 end
            -- No Lua read: the first observer is NGINX add_header's indexed variable.
        end
        if phase == "rewrite" and conf.sdk then
            local request = require("apisix.core.request")
            local tracer = require("apisix.tracer")
            local span = tracer.start(ctx.ngx_ctx, "probe.sdk", tracer.kind.internal)
            span:set_attributes({key = "probe", value = "native"})
            span:set_status(tracer.status.ERROR, "probe status")
            span:finish(ctx.ngx_ctx)
            assert(span.start_time and span.end_time and span.attributes[1].key == "probe")
            assert(span.status.code == tracer.status.ERROR and span.status.message == "probe status")
            local up_util = require("apisix.utils.upstream")
            local literal = {host = "127.0.0.1", port = 18881, metadata = {key = "same"}}
            local domain = {host = "backend.native.test", port = 18881, metadata = literal.metadata}
            local resolved = up_util.parse_domain_for_nodes({domain, literal})
            assert(#resolved == 2 and resolved[2] == literal)
            assert(resolved[1] ~= domain and resolved[1].host == "127.0.0.1")
            assert(resolved[1].domain == domain.host and resolved[1].metadata == domain.metadata)
            local resource = require("apisix.resource")
            local source = resource.fetch_latest_conf("/routes/dns")
            local up = {value = source.value.upstream}
            assert(up.value.resource_key == "/routes/dns" and up.value.resource_version == 1)
            assert(up.value.nodes == up.value.dns_nodes and source.has_domain)
            up.value.nodes = {}
            assert(up_util.parse_domain_in_up(up) == up)
            assert(up.value.nodes[1].host == "127.0.0.1" and resource.get_nodes_ver("/routes/dns") == 1)
            assert(up_util.parse_domain_in_up(up) == up and resource.get_nodes_ver("/routes/dns") == 1)
            local vars = ctx.var
            assert(vars.arg_a == "x y")
            assert(vars.native_yield == "yielded" and vars.native_yield == "yielded")
            assert(ctx.getter_calls == 1)
            assert(vars.native_live == 1 and vars.native_live == 2)
            assert(vars.native_false == false and vars.native_false == false)
            assert(ctx.false_getter_calls == 1)
            local utils = require("apisix.core.utils")
            local saved_info = vars.rate_limiting_info
            local key = 'key"\\\n' .. string.char(0) .. "中"
            local detail = {window_type = "fixed", window_size = 60, decision = "allowed",
                            cost = 1, now = 117.2, window_end = 120.125, count = 3, created = false}
            utils.set_var_rate_limiting_info(ctx, key, 10, "7", 3, detail)
            local rate_info = require("apisix.core.json").decode(vars.rate_limiting_info)
            assert(rate_info.rate_limiting_key == key and rate_info.rate_limiting_limit == 10)
            assert(rate_info.rate_limiting_remaining == 7 and rate_info.evaluated_at_ms == 117200)
            assert(rate_info.current_window.start_ms == 60125 and rate_info.current_window.end_ms == 120125)
            assert(rate_info.current_window.created == false and rate_info.current_window.count == 3)
            assert(vars.rate_limiting_info == ngx.var.rate_limiting_info)
            detail.count, detail.created = 4, true
            utils.set_var_rate_limiting_info(ctx, key, 10, 6, 3, detail)
            rate_info = require("apisix.core.json").decode(vars.rate_limiting_info)
            assert(rate_info.rate_limiting_remaining == 6 and rate_info.current_window.count == 4)
            assert(rate_info.current_window.created == true, "stale rate info cache")
            for _, variant in ipairs({
                {window_type = "fixed", window_size = 60, decision = "allowed", cost = 1,
                 now = 117.2, window_end = 120.125, count = 3, created = false},
                {window_type = "fixed", window_size = 60, decision = "rejected"},
                {window_type = "fixed", window_size = 60, decision = "error"},
                {window_type = "fixed", window_size = 60, decision = "allowed", cost = 0.5},
                {window_type = "fixed", window_size = 60, decision = "allowed", count = 1e14},
                {window_type = "fixed", window_size = 0.001, decision = "allowed",
                 now = 0.0015, window_end = 0.0015, count = false, created = true},
            }) do
                local reference, general = {var = {}}, setmetatable({}, {})
                for field, value in pairs(variant) do general[field] = value end
                -- A metatable keeps the independent table/cjson encoder as an oracle.
                utils.set_var_rate_limiting_info(reference, key, 10, "7", 3, general)
                utils.set_var_rate_limiting_info(ctx, key, 10, "7", 3, variant)
                local decode = require("apisix.core.json").decode
                assert(require("apisix.core.table").deep_eq(decode(reference.var.rate_limiting_info),
                                                           decode(vars.rate_limiting_info)))
            end
            local decode = require("apisix.core.json").decode
            -- The stable plugin uses the basic shape without detail.
            utils.set_var_rate_limiting_info(ctx, key, 10, "7", 3)
            local plain = decode(ngx.var.rate_limiting_info)
            assert(plain.rate_limiting_key == key and plain.rate_limiting_remaining == 7)
            assert(plain.window_type == nil and plain.current_window == nil)
            assert(vars.rate_limiting_info == ngx.var.rate_limiting_info)
            -- Snapshot input by value; mutation after the API call cannot affect it.
            detail.count = 11
            utils.set_var_rate_limiting_info(ctx, key, 10, 6, 3, detail)
            detail.count = 999
            collectgarbage("collect")
            assert(decode(ngx.var.rate_limiting_info).current_window.count == 11)
            -- A direct NGINX assignment before the first read cancels pending JSON.
            utils.set_var_rate_limiting_info(ctx, key, 10, 5, 3, detail)
            ngx.var.rate_limiting_info = "explicit"
            assert(ngx.var.rate_limiting_info == "explicit" and vars.rate_limiting_info == "explicit")
            utils.set_var_rate_limiting_info(ctx, key, 10, 4, 3, detail)
            vars.rate_limiting_info = false
            assert(vars.rate_limiting_info == false and ngx.var.rate_limiting_info == "false")
            utils.set_var_rate_limiting_info(ctx, key, 10, 3, 3, detail)
            ngx.var.rate_limiting_info = nil
            assert(ngx.var.rate_limiting_info == nil and vars.rate_limiting_info == nil)
            -- Direct NGINX writes keep an already-published core.ctx cache unchanged.
            utils.set_var_rate_limiting_info(ctx, key, 10, 3, 3)
            local cached_info = vars.rate_limiting_info
            ngx.var.rate_limiting_info = "external-after-cache"
            assert(vars.rate_limiting_info == cached_info)
            assert(ngx.var.rate_limiting_info == "external-after-cache")
            -- Multiple unread updates, a long key, and generic fallback invalidate both views.
            utils.set_var_rate_limiting_info(ctx, "superseded", 10, 2, 3, detail)
            local long_key = string.rep(key, 400)
            utils.set_var_rate_limiting_info(ctx, long_key, 10, 1, 3)
            assert(decode(ngx.var.rate_limiting_info).rate_limiting_key == long_key)
            detail.cost = 0.5
            utils.set_var_rate_limiting_info(ctx, key, 10, 0, 3, detail)
            assert(decode(vars.rate_limiting_info).cost == 0.5)
            vars.rate_limiting_info = saved_info
            local cached = require("native_cache_probe")("shared", 1, function()
                ngx.sleep(0.001)
                return {value = "factory yielded"}
            end)
            assert(cached.value == "factory yielded")
            local plugin = require("apisix.plugin")
            if ctx.consumer_name then
                assert(ctx.conf_type:find("&consumer", 1, true), ctx.conf_type)
            end
            assert(request.header(ctx, "X-Test") == "original")
            local headers = request.headers(ctx)
            assert(request.headers(ctx) == headers)
            request.set_header(ctx, "X-Test", "modified")
            assert(request.headers(ctx) == headers and headers["x-test"] == "modified")
            assert(request.header(ctx, "x-test") == "modified")
            assert(vars.http_x_test == "modified")
            request.set_header(ctx, "X-Test", "changed-again")
            assert(vars.http_x_test == "changed-again", "stale cached header variable")
            request.set_header(ctx, "X-Test", "modified")
            assert(vars.http_x_test == "modified")
            local websocket = require("apisix.core.websocket").get_role("client")
            local frame = {payload = "before", code = 1000}
            websocket.stash_frame(frame)
            assert(websocket.get_frame() == frame)
            websocket.set_frame_data("after")
            websocket.set_status(1001)
            assert(frame.payload == "after" and frame.code == 1001)
            request.add_header(ctx, "X-Test", "second")
            assert(request.headers(ctx) == headers and headers["x-test"][2] == "second")
            assert(request.header(ctx, "x-test") == "modified")
            assert(request.headers(ctx)["x-test"][2] == "second")
            local args = request.get_uri_args(ctx)
            assert(request.get_uri_args(ctx) == args and args.a[1] == "x y")
            local body, err, kind = request.get_body(conf.max or 1024, ctx)
            if conf.small then
                assert(body == nil and kind == "too_large", tostring(err))
            else
                assert(body == conf.body, tostring(err))
                if conf.form then
                    assert(request.get_post_args(ctx).k[1] == "one two")
                    assert(vars.post_arg_k == "one two")
                elseif conf.json then
                    local parsed = assert(request.get_request_body_table(ctx))
                    assert(parsed.answer == 42)
                    assert(request.get_json_request_body_table() == parsed)
                    local different, message = request.get_request_body_table(ctx, "multipart/form-data")
                    assert(different == nil and message:find("type mismatch", 1, true))
                end
            end
        end
        if phase == "access" and conf.reject then return 418, {message = "probe rejected"} end
        if phase == "before_proxy" and name == "global" and ctx.upstream_conf then
            local actual = ngx.get_phase()
            ctx.probe_balance_calls = (ctx.probe_balance_calls or 0) + 1
            assert(actual == (ctx.probe_balance_calls == 1 and "access" or "balancer"), actual)
            assert(ctx.balancer_ip == "127.0.0.1" and ctx.balancer_port)
            assert(ctx.var.upstream_scheme == ngx.var.upstream_scheme)
            assert(ctx.var.upstream_host == ngx.var.upstream_host)
        end
        if phase == "before_proxy" and conf.wire then
            local view = {}
            for _, key in ipairs({"uri", "request_uri", "real_request_uri", "request_line",
                    "original_x_forwarded_for", "original_x_forwarded_host", "original_x_forwarded_proto",
                    "original_x_forwarded_port", "original_forwarded", "http_x_forwarded_proto",
                    "http_x_forwarded_host", "http_x_forwarded_port", "http_forwarded", "http_x_forwarded_for"}) do
                view[key] = ctx.var[key]
            end
            view.matched = ctx.curr_req_matched
            local request = require("apisix.core.request")
            request.set_header(ctx, "X-Core-View", engine.encode(view))
        end
        if phase == "header_filter" then
            ngx.header["X-Probe-" .. name] = conf.tag or name
        end
        if phase == "access" and ctx.consumer_name then
            assert(ctx.conf_type == "global_rule" or ctx.conf_type:find("consumer", 1, true), ctx.conf_type)
            if name == "global" then
                local versions = package.loaded.native_consumer_route_probe or {}
                package.loaded.native_consumer_route_probe = versions
                local key = ctx.route_id .. "#" .. ctx.consumer_name .. "#" .. ctx.matched_route.modifiedIndex
                if versions[key] then assert(versions[key] == ctx.matched_route, "merged route identity changed") end
                versions[key] = ctx.matched_route
            end
        end
        if phase == "log" then
            ngx.shared.probes:set("trace:" .. ngx.var.request_uri, engine.encode(ctx.trace))
        end
    end
    for _, phase in ipairs({"rewrite", "access", "before_proxy", "header_filter",
                           "body_filter", "delayed_body_filter", "log"}) do
        plugin[phase] = function(conf, ctx) return record(conf, ctx, phase) end
    end
    return plugin
end

return M
