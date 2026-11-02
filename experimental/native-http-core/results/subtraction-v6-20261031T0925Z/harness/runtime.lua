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
    local retry, code, body = engine.balance_enter()
    if code ~= nil then return emit(code, body) end
    if retry then M.run("before_proxy") end
    return emit(engine.balance_finish())
end

return M
