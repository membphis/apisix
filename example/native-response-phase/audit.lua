-- This runs in the NEW native response context, after the Nginx proxy has
-- received the body and before ANY response header filter has been called.
assert(ngx.get_phase() == "response")
assert(not ngx.headers_sent)
ngx.ctx.native_phase = ngx.get_phase()
local body = ngx.response.get_body()

local function reject(status, message)
    ngx.response.clear_headers()
    ngx.status = status
    ngx.response.set_body(message)
end

if body == "LUA_ERROR\n" then
    error("intentional response-stage exception")
end
if body == "LOCAL_SLEEP\n" then
    ngx.sleep(1)
    assert(ngx.get_phase() == "response" and not ngx.headers_sent)
    ngx.log(ngx.INFO, "native_response_lua resumed_after_sleep phase=", ngx.get_phase())
    return
end
if body == "OUTPUT_GUARD\n" then
    for _, api in ipairs({ngx.print, ngx.flush, ngx.send_headers}) do
        local ok = pcall(api, "must not reach client")
        assert(not ok, "output API unexpectedly enabled")
    end
    return
end

local httpc = require("resty.http").new()
-- Deliberately longer than the native phase's 1500ms absolute deadline:
-- the C timer must cancel a stalled or trickling reviewer, including its socket.
httpc:set_timeout(5000)
local ok = httpc:connect({scheme = "http", host = "127.0.0.1", port = 1981})
if not ok then
    httpc:close()
    return reject(502, "audit failure\n")
end
local res = httpc:request({method = "POST", path = "/review", body = body,
    headers = {Host = "localhost", ["Content-Type"] = "text/plain", Connection = "close"}})
if not res or res.status ~= 200 then
    httpc:close()
    return reject(502, "audit failure\n")
end
local chunks, length = {}, 0
while true do
    local chunk, err = res.body_reader(math.min(32, 65 - length))
    if err then
        httpc:close()
        return reject(502, "audit failure\n")
    end
    if not chunk then break end
    length = length + #chunk
    if length > 64 then
        httpc:close()
        return reject(502, "audit failure\n")
    end
    chunks[#chunks + 1] = chunk
end
httpc:close()
assert(ngx.get_phase() == "response" and not ngx.headers_sent)
local decision = table.concat(chunks)
if decision == "deny\n" then
    return reject(403, "response rejected by audit\n")
end
if decision ~= "allow\n" then
    return reject(502, "audit failure\n")
end
