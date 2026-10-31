-- Instrument the existing APISIX filter; leave its behavior in place.
local apisix = require("apisix")
local original = apisix.http_header_filter_phase
apisix.http_header_filter_phase = function()
    if ngx.var.native_response_enabled == "1" then
        ngx.log(ngx.INFO, "native_header_observer phase=", ngx.get_phase(),
                " status=", ngx.status, " upstream_status=", ngx.var.upstream_status)
        ngx.header["X-Native-Phase"] = ngx.ctx.native_phase or "not-entered"
        ngx.header["X-Header-Observer"] = tostring(ngx.status)
        ngx.header["X-Native-Upstream-Status"] = ngx.var.upstream_status
    end
    return original()
end
