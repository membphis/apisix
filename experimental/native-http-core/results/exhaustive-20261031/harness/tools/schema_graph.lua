-- SPDX-License-Identifier: Apache-2.0
-- Build-time extraction of schema DATA from the pinned source. No source
-- functions are included or loaded by the native runtime.
package.cpath = "/usr/local/openresty/lualib/?.so;" .. package.cpath
local json = require("cjson.safe")
if arg[2] == "redis" then
    local function copy(t)
        if type(t) ~= "table" then return t end
        local result = {}
        for k, v in pairs(t) do result[k] = copy(v) end
        return result
    end
    package.preload["apisix.core"] = function() return {table = {deepcopy = copy}} end
end
local ids, nodes = {}, {}
local function scalar(value)
    if type(value) == "table" then
        if not ids[value] then
            ids[value] = #nodes + 1
            local entries = {}
            nodes[#nodes + 1] = entries
            local keys = {}
            for key, val in pairs(value) do
                if type(val) ~= "function" then keys[#keys + 1] = key end
            end
            table.sort(keys, function(a, b)
                if type(a) ~= type(b) then return type(a) < type(b) end
                return a < b
            end)
            for _, key in ipairs(keys) do
                entries[#entries + 1] = {scalar(key), scalar(value[key])}
            end
        end
        return {"ref", ids[value]}
    end
    assert(type(value) == "string" or type(value) == "number" or type(value) == "boolean")
    return {type(value), value}
end
local root = scalar(dofile(arg[1]))
print(assert(json.encode({root = root[2], nodes = nodes})))
