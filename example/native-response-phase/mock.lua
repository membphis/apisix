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

local dict = ngx.shared.audit_demo
local path = ngx.var.uri

if path == "/stats" then
    local stats = {worker_pid = ngx.worker.pid()}
    for _, kind in ipairs({"upstream", "audit"}) do
        for _, key in ipairs({"started", "active", "done", "cancelled"}) do
            stats[kind .. "_" .. key] = dict:get(kind .. "_" .. key) or 0
        end
    end
    ngx.header.content_type = "application/json"
    ngx.say(require("cjson").encode(stats))
    return
end

if path == "/plain" then
    ngx.say("plain ok")
    return
end

local kind = ngx.var.server_port == "1981" and "audit" or "upstream"
ngx.ctx.demo_kind = kind
dict:incr(kind .. "_started", 1, 0)
dict:incr(kind .. "_active", 1, 0)

if kind == "audit" then
    if path ~= "/review" or ngx.req.get_method() ~= "POST" then
        return ngx.exit(404)
    end
    ngx.req.read_body()
    local body = ngx.req.get_body_data() or ""
    -- Both normal verdicts wait exactly one second without blocking the worker.
    if body:find("AUDIT_TRICKLE", 1, true) then
        ngx.print("a")
        ngx.flush(true)
        for _ = 1, 10 do
            ngx.sleep(0.3)
            ngx.print("x")
            ngx.flush(true)
        end
    else
        ngx.sleep(body:find("AUDIT_TIMEOUT", 1, true) and 3 or 1)
        if body:find("AUDIT_DISCONNECT", 1, true) then
            return ngx.exit(444)
        elseif body:find("AUDIT_ERROR", 1, true) then
            ngx.status = 500
            ngx.say("reviewer unavailable")
        elseif body:find("AUDIT_INVALID", 1, true) then
            ngx.say("not a verdict")
        elseif body:find("AUDIT_OVERSIZE", 1, true) then
            ngx.print(string.rep("x", 65))
        else
            ngx.say(body:find("DENY_ME", 1, true) and "deny" or "allow")
        end
    end
    ngx.ctx.demo_done = true
    return
end

local case = path:match("^/native/(.+)$")
if not case then
    return ngx.exit(404)
end
ngx.status = 201
ngx.header["Content-Type"] = "text/plain; charset=utf-8"
ngx.header["X-Origin"] = "retained"
ngx.header["Set-Cookie"] = {"demo_a=1", "demo_b=2"}
ngx.header["Connection"] = "close, X-Hop"
ngx.header["X-Hop"] = "must-not-forward"

local bodies = {
    allow = "hello from upstream\n",
    deny = "DENY_ME: never expose this text\n",
    utf8 = "小文本审核通过\n",
    empty = "",
    sleep = "LOCAL_SLEEP\n",
    ["lua-error"] = "LUA_ERROR\n",
    ["output-guard"] = "OUTPUT_GUARD\n",
    ["status-404"] = "small upstream not found\n",
    ["status-503"] = "small upstream unavailable\n",
    exact = string.rep("e", 4096),
    ["large-length"] = "DO_NOT_LEAK" .. string.rep("x", 4096),
    timeout = "AUDIT_TIMEOUT: never expose this text\n",
    ["audit-error"] = "AUDIT_ERROR: never expose this text\n",
    ["audit-invalid"] = "AUDIT_INVALID: never expose this text\n",
    ["audit-oversize"] = "AUDIT_OVERSIZE: never expose this text\n",
    ["audit-trickle"] = "AUDIT_TRICKLE: never expose this text\n",
    ["audit-disconnect"] = "AUDIT_DISCONNECT: never expose this text\n",
}
if case == "chunked" or case == "deny-chunked"
   or case == "large-chunked" or case == "slow-upstream" then
    local body = case == "large-chunked" and "DO_NOT_LEAK" .. string.rep("x", 4096)
                 or case == "deny-chunked" and "hello then DENY_ME: never expose this text\n"
                 or "hello from upstream\n"
    ngx.print(body:sub(1, 6))
    ngx.flush(true)
    ngx.sleep(case == "slow-upstream" and 3 or 0.15)
    ngx.print(body:sub(7))
elseif case == "upstream-trickle" then
    for _ = 1, 20 do
        ngx.print("x")
        ngx.flush(true)
        ngx.sleep(0.2)
    end
elseif case == "broken" then
    ngx.header["Content-Length"] = 100
    ngx.print("partial DO_NOT_LEAK")
elseif case == "disconnect" then
    return ngx.exit(444)
elseif case == "not-text" then
    ngx.header["Content-Type"] = "application/octet-stream"
    ngx.print("DO_NOT_LEAK")
elseif case == "encoded" then
    ngx.header["Content-Encoding"] = "gzip"
    ngx.print("DO_NOT_LEAK")
elseif bodies[case] then
    if case == "status-404" then
        ngx.status = 404
    elseif case == "status-503" then
        ngx.status = 503
    end
    ngx.header["Content-Length"] = #bodies[case]
    ngx.print(bodies[case])
else
    ngx.status = 500
    ngx.say("unknown mock case")
end
ngx.ctx.demo_done = true
