-- SPDX-License-Identifier: Apache-2.0
-- Diagnostic scopes for the no-yield, static GET/local-limit scenario only.
local ffi = require("ffi")
ffi.cdef[[
int ac_cost_tag(void);
int ac_cost_enter(int tag);
void ac_cost_leave(int previous);
void ac_cost_begin(uintptr_t request, int is_new);
void ac_cost_end(void);
void ac_cost_finish(uintptr_t request);
void ac_cost_spin(uint64_t nanoseconds);
]]
local C = ffi.C
local get_request = require("resty.core.base").get_request
local sampler = assert(package.loadlib("/tmp/core-cost-sampler.so", "luaopen_core_cost_sampler"))()
local M = {sampler = sampler}
local verification = false
local function plugin_invoke(callback, ...)
    if verification then
        local spin = tonumber(ngx.var.arg_cost_plugin_ns)
        if spin then C.ac_cost_spin(spin) end
    end
    return callback(...)
end
local function sdk_invoke(callback, name, ...)
    if verification and name == "set_var_rate_limiting_info" then
        local spin = tonumber(ngx.var.arg_cost_sdk_ns)
        if spin then C.ac_cost_spin(spin) end
    end
    return callback(...)
end
local function finish_scope(previous, ...)
    C.ac_cost_leave(previous)
    return ...
end
local function wrap_scope(callback, tag)
    return function(...)
        if C.ac_cost_tag() == 0 then return callback(...) end
        return finish_scope(C.ac_cost_enter(tag), plugin_invoke(callback, ...))
    end
end
local function phase_done(id, final, ...)
    C.ac_cost_end()
    if final then C.ac_cost_finish(id) end
    return ...
end
local function wrap_phase(callback, is_new, final)
    return function(...)
        local id = ffi.cast("uintptr_t", get_request())
        C.ac_cost_begin(id, is_new and 1 or 0)
        return phase_done(id, final, callback(...))
    end
end

function M.prepare()
    -- Install before plugin plans capture access callbacks. No plugin source is edited.
    for index, loader in ipairs(package.loaders) do
        local current_loader = loader
        package.loaders[index] = function(name)
            local load = current_loader(name)
            if name ~= "apisix.plugins.limit-count" or type(load) ~= "function" then return load end
            return function(...)
                local module = load(...)
                module.access = wrap_scope(module.access, 2)
                return module
            end
        end
    end
end

function M.install(mode, verify)
    verification = verify or false
    if require("ngx.process").type() ~= "worker" then return end
    local core = require("apisix.core")
    local plugin = require("apisix.plugin")
    local methods = {
        {core.utils, "set_var_rate_limiting_info"}, {core.utils, "resolve_var"},
        {core.json, "delay_encode"}, {core.response, "set_header"},
        {core.log, "info"}, {core.log, "error"}, {core.log, "warn"},
        {plugin, "conf_version"}, {plugin, "plugin_metadata"},
        {core.lrucache, "plugin_ctx_id"},
    }
    for _, item in ipairs(methods) do
        local object, key = item[1], item[2]
        local original = assert(object[key], key)
        object[key] = function(...)
            if C.ac_cost_tag() ~= 2 then return original(...) end
            return finish_scope(C.ac_cost_enter(1), sdk_invoke(original, key, ...))
        end
    end
    -- A plugin-supplied creator is plugin business even when an SDK cache calls it.
    local creators = setmetatable({}, {__mode = "k"})
    local cache = core.lrucache.plugin_ctx
    core.lrucache.plugin_ctx = function(lru, ctx, key, creator, ...)
        if C.ac_cost_tag() ~= 2 then return cache(lru, ctx, key, creator, ...) end
        local wrapped = creators[creator]
        if not wrapped then wrapped = wrap_scope(creator, 2); creators[creator] = wrapped end
        return finish_scope(C.ac_cost_enter(1), cache(lru, ctx, key, wrapped, ...))
    end
    if mode == "lua" then
        local apisix = require("apisix")
        for _, name in ipairs({"access", "balancer", "header_filter", "body_filter", "log"}) do
            local key = "http_" .. name .. "_phase"
            apisix[key] = wrap_phase(apisix[key], name == "access", name == "log")
        end
    else
        local runtime = require("apisix.native.runtime")
        local run = runtime.run
        runtime.run = function(phase)
            local id = ffi.cast("uintptr_t", get_request())
            C.ac_cost_begin(id, phase == "access" and 1 or 0)
            return phase_done(id, false, run(phase))
        end
        runtime.balance = wrap_phase(runtime.balance, false, false)
        -- Native header/body and cleanup are bracketed in the diagnostic C build.
    end
end
return M
