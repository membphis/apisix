-- SPDX-License-Identifier: Apache-2.0
-- Empty FFI scope shapes: estimates clock/boundary tax, not JIT-wrapper tax.
local ffi = require("ffi")
ffi.load("/tmp/core-cost-sampler.so", true)
ffi.cdef[[
void ac_cost_begin(uintptr_t,int); void ac_cost_end(void); void ac_cost_finish(uintptr_t);
int ac_cost_enter(int); void ac_cost_leave(int);
]]
local sampler = assert(package.loadlib("/tmp/core-cost-sampler.so", "luaopen_core_cost_sampler"))()
local C = ffi.C
local phases, sdk = assert(tonumber(arg[1])), assert(tonumber(arg[2]))
local id = ffi.cast("uintptr_t", 4096)
local function loop(count)
    for _ = 1, count do
        for phase = 1, phases do
            C.ac_cost_begin(id, phase == 1 and 1 or 0)
            if phase == 1 then
                local core = C.ac_cost_enter(2)
                for _ = 1, sdk do
                    local plugin = C.ac_cost_enter(1)
                    C.ac_cost_leave(plugin)
                end
                C.ac_cost_leave(core)
            end
            C.ac_cost_end()
        end
        C.ac_cost_finish(id)
    end
end
assert(sampler.start(1))
loop(1000)
assert(sampler.reset())
loop(10000)
assert(sampler.stop(assert(arg[3])))
