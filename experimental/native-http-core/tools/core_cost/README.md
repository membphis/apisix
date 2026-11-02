# 核心成本诊断：先分归属，再看探针扰动

这套实验记录用户提出的三类指标：请求路径核心 CPU、GC/分配，以及 Latency。
[本次报告](../../results/core-cost-v6-20261031T0757Z/REPORT.md)保留直接观测和限制。
**当前没有证实精确的未插桩核心净收益倍率；不能把近似扣空后的约20%当成结论。**

## 边界

- 仅使用冻结 `a3770f02` 的静态 GET、local limit-count、空全局规则；无 Prometheus/key-auth/AI。
- Lua access/balancer/header/body/log 和 Native access/balancer/log，以及直接 C header/body/cleanup，均有活动区间。
- 插件 access 记为 plugin；插件调用的 SDK 再记回 core。当前路径每请求一次插件、九次 SDK。
- SDK 列表见 hooks.lua，包括 plugin_ctx、conf_version、plugin_metadata、set_var_rate_limiting_info、set_header、日志与 delay_encode。
  cache 调用插件提供的 creator 时重新记为 plugin。当前请求没有其他动态 SDK 分支。
- core 包含区间内同步底层调用和当时实际发生的 GC；网络 I/O、上游等待、NGINX filter 后续处理不计入。
- NGINX/ngx_lua 的 VM 入口/退出、协程建立、通用请求池释放，以及启动、配置轮询和其他后台核心任务没有单独归属到请求 core。
  因而本工具测的是“请求内核心活动区间”，不是所有架构成本的穷尽归因。
- 仅适用本无yield场景；全局 scope 不支持任意协程挂起/恢复。检测交错或未闭合帧即拒绝结果。

## 文件

- sampler.c：线程 CPU/单调墙钟、按请求累加、1/64 请求计时、公开 VM allocator 计数、公开 LuaJIT CPU sampler。
- hooks.lua：不改插件源码的诊断 wrapper，保留完整返回值；预加载前包裹插件，避免 Native 已缓存原函数。
- run.py：固定配置、正式/预检、窗口重置、工件哈希、worker/错误检查。
- load.py：8,000计划请求/秒、32连接，记录计划时间和实际发起时间；处理正常 Connection: close 重连，保留迟到slot。
- calibrate.lua：空 FFI 边界形状，近似估计读时钟税，不校正 JIT wrapper 影响。
- analyze.py：分别呈现原始观测、近似值、GC估计及未插桩延迟；不把它们加成一个虚假的“净核心成本”。

## 四组正式样本

每组 Lua/C 各一个样本，5秒预热、正式60秒、480,000请求，顺序保存在原始日志。

1. control：原二进制/代码，无 sampler 或 wrapper，用于同到达速率的真实端到端延迟。
2. profile：全部探针，用于scope分配量；发现13%–17% worker CPU增量后，保留这组证据。
3. timing：保留scope/抽样时钟，关闭 allocator 拦截和 GC **采样**，正常 GC 仍开启。
4. gc：原核心二进制/代码，无核心/SDK/plugin wrapper、无 allocator 拦截，只做 CPU timer VM状态采样。

每版 timing 样本抽样7,500请求。GC是整个 worker VM 的估计，不是核心因果GC，更不是单次GC pause。
有效信号交付频率可能不同，因此用样本比例乘实测 worker CPU，不用GC样本数乘1ms。
核心 allocator 数据包含VM/JIT及探针影响，是申请事件/字节，不是对象数、RSS或物理malloc。

## 核验与复现

归属预检分别在 plugin、SDK 注入2ms CPU，并在上游增加30ms等待，核对正确扣除/计入；正式样本不启用这些人工工作。
验证 start/stop 与 allocator 恢复、请求帧关闭、完整请求数、相同CPU绑定及实际响应。
8个样本全通过；负载器最初漏处理每连接1000请求后的正常关闭，修正后的40,000请求预检通过。
切换未插桩配置前需先带旧诊断库退出旧master；相关失败预检/日志保留，不混入正式数据。

在独立目录展开冻结提交，参照[版本记录](../../results/core-cost-v6-20261031T0757Z/PROVENANCE.md)。
runner 使用 `/tmp/apisix-v6-limit-only-frozen/experimental/native-http-core`，诊断库/钩子位于 `/tmp`。
固定依赖仍为原OpenResty/runtime。以归档 `diagnostic-module-source/` 构建独立模块，只加两处C文件的边界，
不覆盖原 `/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so`。
sampler 通过 `gcc -shared -fPIC -O2 -Wall -Wextra -Werror` 和固定LuaJIT头编译，以 LD_PRELOAD 导出计量函数。
仅在独立容器使用；全部样本结束后恢复原二进制、移除PRELOAD和临时控制路由。

后续若要可信的净倍率，需要避免改变Lua函数/JIT边界的更低层探针，并对VM入口和GC step做可校准计量。
不要以当前诊断比例替代此前未插桩吞吐结果，也不要将整个LuaJIT C状态当作本C模块CPU。
