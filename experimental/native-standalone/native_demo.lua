--
-- Licensed to the Apache Software Foundation (ASF) under one or more
-- contributor license agreements. See the NOTICE file distributed with
-- this work for additional information regarding copyright ownership.
-- The ASF licenses this file to You under the Apache License, Version 2.0
-- (the "License"); you may not use this file except in compliance with
-- the License. You may obtain a copy of the License at
--
--     http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--
local core = require("apisix.core")
local plugin = require("apisix.plugin")
local tracer = require("apisix.tracer")
local bit = require("bit")
local M = {}
local routes
local global_log
local endpoint

function M.init_worker()
    local f = assert(io.open(ngx.config.prefix() .. "conf/apisix.json", "rb"))
    local snapshot = assert(core.json.decode(f:read("*a")))
    f:close()
    routes = {}
    local globals = assert(snapshot.global_rules[1])
    assert(plugin.plugin_checker(globals))
    global_log = { assert(plugin.get("prometheus")), globals.plugins.prometheus }

    for i, value in ipairs(snapshot.routes) do
        assert(plugin.plugin_checker(value))
        local route = {value = value, key = "/routes/" .. value.id,
                       modifiedIndex = 1}
        plugin.set_plugins_meta_parent(value.plugins, route)
        route.rewrite = {}
        route.access = {}
        if value.plugins["key-auth"] then
            route.rewrite = {assert(plugin.get("key-auth")), value.plugins["key-auth"]}
        end
        if value.plugins["limit-count"] then
            route.access = {assert(plugin.get("limit-count")), value.plugins["limit-count"]}
        end
        route.matched = {_path = value.uri}
        routes[i] = route
        endpoint = next(value.upstream.nodes)
    end
end

local function context()
    local ctx = ngx.ctx.api_ctx
    if ctx then
        return ctx
    end
    ctx = core.tablepool.fetch("api_ctx", 0, 16)
    ctx.ngx_ctx = ngx.ctx
    ngx.ctx.api_ctx = ctx
    core.ctx.set_vars_meta(ctx)
    local index = tonumber(ngx.var.apisix_native_route_index)
    if index then
        local route = assert(routes[index], "native route index out of range")
        assert(route.value.id == ngx.var.apisix_native_route_id,
               "native and Lua snapshots disagree")
        ctx.matched_route = route
        ctx.route_id = route.value.id
        ctx.curr_req_matched = route.matched
        ctx.conf_type = "route"
        ctx.conf_id = route.value.id
        ctx.conf_version = route.modifiedIndex
    end
    return ctx
end

function M.access()
    local ctx = context()
    if not ctx.matched_route then
        return core.response.exit(404, {error_msg = "404 Route Not Found"})
    end
    local plan = assert(tonumber(ngx.var.apisix_native_route_plan))
    local route = ctx.matched_route
    if bit.band(plan, 1) ~= 0 then
        plugin.run_plugin("rewrite", route.rewrite, ctx)
    end
    if bit.band(plan, 2) ~= 0 then
        plugin.run_plugin("access", route.access, ctx)
    end
    ctx._apisix_proxied = true
    ctx.balancer_ip, ctx.balancer_port = endpoint:match("^([^:]+):(%d+)$")
end

function M.log()
    if ngx.var.apisix_native_global_plan ~= "1" then
        return
    end
    local ctx = context()
    ctx.var.apisix_upstream_response_time = ngx.var.upstream_response_time
    plugin.run_plugin("log", global_log, ctx)
    tracer.finish_all(ctx.ngx_ctx)
    tracer.release(ctx.ngx_ctx)
    core.ctx.release_vars(ctx)
    ctx.ngx_ctx = nil
    core.tablepool.release("api_ctx", ctx)
    ngx.ctx.api_ctx = nil
end

return M
