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
local engine = require("apisix.native.engine")
local M = {}

local function resume_call(resume, ...)
    if resume == nil then return ... end
    local callback, args = ...
    return resume_call(resume(pcall(callback, unpack(args, 1, args.n))))
end

-- Generic coroutine bridge. Native continuations own policy and state; this
-- frame only invokes a callable and transports its complete result tuple.
function M.bind(start)
    return function(...)
        return resume_call(start(...))
    end
end

-- Replay values already published by C without leaving a compiled plugin
-- trace. C owns lookup, cache population, invalidation and uncached getters.
function M.cached_index(callback)
    return function(view, key)
        if type(key) == "string" then
            local value = view._cache[key]
            if value ~= nil then return value end
        end
        return callback(view, key)
    end
end

-- Optional framework cooperation: replay only a C-published raw cache entry.
-- The runtime dirty bit is read through its existing traceable FFI wrapper;
-- native code still owns cache creation, refresh, and mutation handling.
function M.cached_headers(callback)
    local changed = require("resty.apisix.request").is_request_header_set
    return function(ctx)
        if type(ctx) == "table" then
            local headers = rawget(ctx, "headers")
            if type(headers) == "table" and not changed() then return headers end
        end
        return callback(ctx)
    end
end

function M.cached_header(callback)
    local changed = require("resty.apisix.request").is_request_header_set
    return function(ctx, name)
        if type(ctx) == "table" then
            local headers = rawget(ctx, "headers")
            if type(headers) == "table" then
                -- A miss may invoke a user metatable. Leave it to the original
                -- path so probing the cache cannot run that getter twice.
                local value = rawget(headers, name)
                if value ~= nil and not changed() then
                    return type(value) == "table" and value[1] or value
                end
            end
        end
        return callback(ctx, name)
    end
end

local function run_step(callback, ...)
    if callback == nil then return ... end
    return run_step(engine.step(pcall(callback, ...)))
end

local function emit(code, body)
    if code == nil then return end
    code, body = engine.render(code, body)
    ngx.status = code
    if body ~= nil then ngx.print(body) end
    return ngx.exit(code)
end

function M.run(phase)
    return emit(run_step(engine.start_phase(phase)))
end

function M.balance()
    local retry, code, body = engine.balance_start()
    if code ~= nil then return emit(code, body) end
    if retry == nil then return end
    if retry then M.run("before_proxy") end
    return emit(engine.balance_finish())
end

return M
