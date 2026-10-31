# APISIX next v4 llm：临时研究记录与模型交接

记录日期：2026-10-31（UTC）。这是用户要求随代码提交的临时 Markdown，供后续模型继续研究。
分支：`apisix-next-v4-llm`（用户命名“apisix next v4 llm”的合法 Git 写法）；项目：`membphis/apisix`。
源码基线：`b31e828c6f6b588ae954bf69c8432c08fb73bb7e`（APISIX 3.19.0 源码）。
这里的 V4 是本次研究分支的命名，不是官方发布版本声明。

## 先读什么

1. 本文：任务上下文、尝试记录、证据强度、后续假设。
2. [SCENARIOS.md](SCENARIOS.md)：场景职责、C 所有权与未实现边界。
3. [RESULTS.md](RESULTS.md)：最终数据、各阶段优化与测量口径。
4. [README.md](README.md)：构建、差分验证、运行和剖析方法。
5. [DESIGN_CONTEXT.md](DESIGN_CONTEXT.md)：之前的设计记录，已保留用户后续缩减范围的说明；以其顶部有效范围为准。
6. [NEXT_MODEL_PROMPT.md](NEXT_MODEL_PROMPT.md)：可直接交给另一个模型的提示词。

## 用户目标与上下文

- 最初想实现 APISIX 核心改为 ngx C module、插件继续使用现有 Lua 的 demo，探索性能和稳定性。
- 用户明确否定仅用一百多行把几个入口封装起来：路由、请求上下文、阶段、调度、合并等底层机制应实际迁到 C。
- 目标是插件不改时优化底层机制，不能把插件业务改成 C 后声称达成目标，也不能用旧 Lua 核心完成实际业务。
- 首先讨论架构与收益，再采用 standalone HTTP 开始；使用最少配置和实际代理场景。
- Prometheus 全局开启；key-auth、local limit-count 只在特定前缀路由开启，并测试各自与组合。
- 吞吐压力测试每个样本一分钟足够。先实测再判断，不预设提升百分比，不自动扩大为多轮或长期压力矩阵。
- 用户后来明确缩减实施范围：健康检查、慢启动、服务发现不用实现；只完成场景直接用到的能力，其余记录扩展点即可。
- 已有 DNS、脚本、哈希、热更新和额外路由实现保留其回归，但不因此继续扩大交付范围。
- 用户认为 C 的 CPU/内存效率应有优势，并把早期极简原型约 40%–50% 的提升视为可能的优化空间参考。
- 应回应这一动机，但早期原型省略了核心职责且保留旧 Lua SDK，其数字不能直接当作完整实现的理论上限或保证。
- 用户最后提供的具体思路：`upstream_response_time` 等变量可直接调用第三方库暴露的 C API；用显式 if/else 读取也可以。
- 已落实上述直接调用，并继续分析语言边界、对象创建和 JIT 行为。当前结果是正收益，但没有达到 40%–50%。
- 用户现在要求把代码、优化过程、上下文和下一步思路提交到自己的特定分支，并准备让另一个模型继续深挖。

## 实现归属与代码地图

当前核心共 24 个 C 源文件、9,388 行（含注释和空行；不含生成头、测试剖析器）。行数只说明规模，不证明兼容性。
原 `apisix/` 目录及 Lua 插件、插件辅助文件与源码基线一致。

| 职责 | 文件 |
| --- | --- |
| NGINX 模块注册、初始化、请求池/清理、Lua 引用管理 | `module/module.c`、`module/engine.h` |
| 路由树、优先级、host/method/CIDR/参数匹配与表达式 | `module/routes.c`、`module/expression.c` |
| 插件注册、全局/路由执行计划、阶段状态机与异常处理 | `module/plugins.c` |
| Route/Service/PluginConfig/Consumer/Group 合并、版本及元数据 | `module/merge.c`、`module/consumer.c`、`module/resources.c`、`module/cache.c` |
| 请求变量、头/body、响应 SDK 及运行时适配 | `module/request.c`、`module/http.c`、`module/response.c`、`module/sdk.c` |
| 纯 C header/body 过滤阶段 | `module/native_phases.c` |
| 上游准备、选点、超时、重试、连接复用 | `module/upstream.c`、`module/upstream_helpers.c`、`module/resolver.c` |
| 配置事务发布、在途代际 pin/release、文件监视 | `module/snapshot.c` |
| schema、工具、指标、后台/平台接口、追踪和用户脚本调度 | 其余 `module/*.c`；具体边界见 SCENARIOS/README |
| 协程返回值传输、回放 C 已发布的变量缓存 | `runtime.lua`，68 行，保留 Lua 运行时必要桥接 |
| 真插件场景、SDK/阶段探针与六种核心配置回归 | `tests/` |
| 构建、运行、对照、测量、剖析与报告生成 | `tools/` |

运行时通过原有 `require("apisix.*")` 名称预加载 C 实现，保留插件可见的 Lua table ABI。
原版 Lua 核心加载守卫拒绝非插件核心模块，并检查函数来源，不能靠换入口名绕过守卫。
NGINX/OpenResty 负责 HTTP 传输、事件和连接；ngx_lua/LuaJIT 及第三方库继续作为运行时。
APISIX CLI 只在准备阶段生成共同模板/导出默认配置，不代表 native 请求仍执行旧 Lua 核心。

常规请求阶段仍需 ngx_lua 合法协程入口和薄 Lua 传输帧。C 决定路由、计划、进度、缓存和上游；Lua 执行未改的回调。
没有 Lua filter 回调、脚本或 tracing 的本次配置启用直接 NGINX C header/body 过滤链。
通用 Lua filter 回调路径仍保留并单独回归；直接过滤模式遇到不支持的组合会失败关闭。

首次 `before_proxy` 在 access 中完成上游准备/选点后调用；重试时在 balancer 重新选点后调用。
不要为了省一次边界而改变插件观察到的阶段、变量或执行顺序。

## 已实施的优化：原因、方法、限制

以下是完整尝试链条中的主要变更。并非每一项都有独立的一分钟 A/B 消融实验，不能把总收益拆成精确的单项贡献。

| 变更 | 原因与做法 | 语义或证据要求 |
| --- | --- | --- |
| 去除通用桥接的临时 `pack` 表 | LuaJIT 采样看到解释执行/GC 和 pack；直接传输 varargs 与完整返回值元组 | nil/false、返回值数量、异常及合法 yield 都保留 |
| 模块查找缓存 | 避免热路径反复 require/取成员 | 不绕过模块来源守卫 |
| Consumer 合并配置/执行计划缓存 | 原先逐请求复制并编译；按配置代际及消费者/组缓存 | 保持对象身份与版本失效；最多 256 个组合，超出使用请求内计划 |
| 无回调阶段快速返回 | 场景没有对应回调仍有多次语言切换 | 仍执行该阶段所需核心逻辑 |
| 直接变量读取 | `request.c: ac_native_var_fetch` 对已知变量调用导出的 typed C 原语，其他走 NGINX 变量 API | 类型、重试计时聚合、缓存/自定义 getter 语义不变 |
| 直接变量写入与标量 header | 调用 `ngx_http_lua_ffi_var_set`、req/resp header C API，减少 Lua/FFI 中转 | 多值 header 保留通用路径；注意请求池复制与变量缓存失效 |
| 上游直接 C API | timeout、peer、tries、recreate、failure、keepalive 直接调用导出原语 | 连接池 key/CRC/size、超时及参数保持一致，真实连接 ID 验证复用 |
| 阶段转换合并 | 完成回调、选下一项、阶段结束合成 `start_phase`/`step` 的 C 转换 | Lua 仅 pcall 回调并传输结果；不得破坏 yield/恢复 |
| 配置上下文元数据缓存 | 函数剖析显示每请求多次 `ac_route_context`/`ac_var_assign` | conf_type/id/version、route/service 名按代际及身份缓存，引用由代际拥有 |
| 表容量预分配 | 请求上下文和限流诊断表减少扩容 | 仍使用原插件可见的 table 结构 |
| 直接 C header/body filters | 三个原插件没有对应回调，移除空 ngx_lua 阶段入口 | C 保留 Server/上游状态头，body 原样传递；拒绝 Lua filter/script/tracing 组合 |
| 固定窗口诊断 JSON 用 C 编码 | `utils.c: ac_rate_fixed` 去掉多层中间 Lua 表与 cjson 路径 | 仅安全整数/固定窗口形状；其余走通用路径；转义、精度、false/null/缺失值做差分 |
| 限流 JSON 的 Lua 字符串延后创建 | 完整 JSON 仍写入 NGINX 变量，插件真正读取时才创建 Lua view | 现实现仍逐请求 C 编码；尚未把完整 JSON 编码本身改为按需执行 |
| C 批量准备日志变量 | `request.c: ac_prefetch_log_vars` 一次准备稳定 HTTP/指标字段，减少 Lua 逐字段往返 | 已有缓存值（含 false）优先；自定义 getter 仍按插件读取时执行；tracing 时保留原路径 |
| Lua 只回放 C 已发布缓存 | `runtime.lua: cached_index` 让缓存命中留在可编译 Lua trace | 查找策略、cache admission、失效与未缓存 getter 仍由 C 决定 |
| 二进制原子替换 | 重新编译时写 `.so.new` 后 replace，避免覆盖旧 worker mmap 的 inode | 这是构建/运行可靠性修正，不算吞吐优化 |
| 测量与报告防混用 | 拒绝 sanitizer/函数剖析构建；报告校验 C 源码和 runtime.lua SHA | 防止拿旧轮数据给新源码背书 |

具体时间变量使用 `ngx_http_lua_var_ffi_upstream_response_time(r, buf, type)`，不同 type 对应 response/header/connect timings。
直接适配依赖固定运行时的导出 ABI，没有绑定私有 ngx_lua 结构布局；升级运行时仍需核对签名、重编译和回归。

## 历次完整矩阵：保留负收益

下面各行是同轮 Lua/C 的吞吐变化。轮次标识仅用于研究记录，不是产品版本。

| 阶段/证据目录 | 基础 | 认证 | 限流 | 组合 |
| --- | ---: | ---: | ---: | ---: |
| 初始完整 C 场景：`results/before-optimization/` | -29.0% | -34.5% | -30.5% | -39.8% |
| 桥接与合并缓存后：`results/before-native-access/` | -10.3% | -14.2% | -18.9% | -21.0% |
| 直接 API、阶段合并、元数据缓存：`results/before-native-filters/` | +2.3% | -0.6% | -3.2% | -10.5% |
| C filters、C JSON 后：`results/before-log-prefetch/` | -4.4% | +7.4% | +7.6% | +2.9% |
| 日志批量准备后最终轮：`results/` | +9.9% | +1.4% | +11.1% | +9.2% |

早期 `../native-standalone/` 是另一份局部原型：267 行 C，仍运行旧 Lua SDK/部分核心且省略职责。
保留它及其原始结果作为历史对照，不能将该原型的约 40%–50% 提升套用到当前实现。

最终轮绝对数值：

| 场景（均有全局 Prometheus） | Lua QPS | C QPS | Lua CPU μs/请求 | C CPU μs/请求 |
| --- | ---: | ---: | ---: | ---: |
| 基础代理 | 17,908.72 | 19,674.69 | 55.80 | 50.76 |
| key-auth | 17,236.36 | 17,472.10 | 57.97 | 57.19 |
| local limit-count | 14,671.79 | 16,301.09 | 68.12 | 61.31 |
| 认证＋限流 | 12,873.50 | 14,053.59 | 77.58 | 71.08 |

四场景 CPU/请求低 1.3%–10.0%，结束 worker RSS 低 33.8%–35.6%；八个样本错误率均为 0。
最终模块 SHA-256：`6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8`。
全部 p50/p99/RSS 和原始 wrk 输出在 RESULTS 与 results 中；不要只引用上述表格而忽略测量口径。

## 测量与剖析的方法

### 压测口径

- APISIX 3.19.0 源码搭配固定底图：`apache/apisix:3.14.1-debian@sha256:c228717165ecf4c0055818c9e2d9843f7b303f17a6df9e5fbaf8eafeb5007ae4`。
- 实际共同运行时是 OpenResty 1.27.1.2、ngx_lua 0.10.28、APISIX runtime 1.3.2、GCC 10，模块 `-O2`。
  镜像标签的 APISIX 版本不是本次 Lua 源码对照的版本；依赖与源码哈希见 `results/environment.json`。
- 单网关 worker 绑 CPU 0，上游 worker 绑 CPU 1，wrk 绑 CPU 2–4；共同云机器没有独占性保证。
- wrk 2 线程、32 连接、HTTP/1.1 keepalive，后端返回 3 字节 `ok\n`。
- 四种场景各 Lua/C 一个样本，每样本预热 5 秒、正式运行 60 秒；每秒抓取一次全局 Prometheus。
- 所有压力请求使用有效 key；压测配额 1,000,000,000/600 秒，实际经过认证/限流且代理到上游，不用快速 401/429 增加 QPS。
- 检查 worker PID 与 CPU affinity；重启、非 2xx/3xx、socket error、新增 error/alert 日志均让样本失败。
- CPU/请求只计网关 worker 的 CPU 时间，不含后端、wrk、master 和 privileged agent。
- p99 来自闭环 wrk，各实现在自己的吞吐下运行，不能解读成相同到达率下的尾延迟比较。
- 最终轮是单次配对结果，没有统计置信区间。跨轮基线有明显波动，认证 +1.4% 尤其不足以证明稳定收益。
- 当前脚本固定先 Lua 后 C；下一步可考虑在仍保持每样本一分钟的前提下交替顺序，以识别顺序偏差。
- 每次配置保存在 `*-nginx.conf`、`*-resources.json`；环境、依赖、源码、插件和二进制哈希一并记录。

### 三类剖析

1. `--sampler jit`：LuaJIT 状态/热点采样。曾观察 native 路径 compiled 部分约 11%，原版约 29%，其余 C/解释/GC 边界较多。
   这是旧轮特征，不是最终轮比例，也不能说明某个 C 函数消耗了多少 CPU。
2. `--sampler pc`：独立 `tools/cpu_sampler.c` 收集 PC 与映射，通过 ELF 近邻符号辅助定位。
   它没有调用栈，stripped/IFUNC 依赖的近邻名称可能不准确；不能将采样点当完整火焰图或因果证明。
3. `--sampler functions`：GCC `-finstrument-functions` 与独立 `tools/function_sampler.c` 统计 C 调用次数和子树时间。
   一份记录中 78,228 个 `ac_http_prepare` 对应 625,796 次 `ac_route_context`（约 8 次/请求）、
   1,408,104 次 `ac_var_assign`（约 18 次/请求）；限流诊断是另一热点。
   该构建有显著 instrumentation 开销，C 子树占比不是整个 worker CPU 占比，不能用于验收 QPS。

短剖析通常约 15 秒，只是诊断，不是额外的一分钟吞吐验收样本。
函数剖析后必须以默认 `tools/dev.py` 恢复优化构建；runner/benchmark 会拒绝带 sanitizer 或函数剖析符号的模块。

## 正确性证据与容易回退的语义

- 六个核心配置 profile 共 1,955 次 HTTP 检查，另有 SDK 断言。profile：default、servlet、tail-slash、encoded-slash、trusted、untrusted。
- 原版 Lua、native 直接 C filters、native 通用回调桥各跑同一套 34 个真实请求；状态指标精确增量为
  21×200、3×404、6×401、2×429、1×503、1×504。
- 验证 URI/前缀边界、body/HEAD、header/query 认证、隐藏凭证、身份配额隔离、超时、错误及真实上游连接复用。
- 双 worker 共享配额在两种实现均为 5 个接受、27 个拒绝，不能用每 worker 独立计数替代共享语义。
- 插件 yield、完整参数/返回值、nil/false、执行顺序、早退/异常、配置共享身份与版本失效均有断言。
- 配置事务发布后，在途请求 pin 旧代际；非法 JSON/schema/filter 不替换旧配置；测试 reader 更新/关闭和旧快照回收。
- Prometheus 的额外真实标签检查把缓存 status 设为 299、llm_model 设为 false，并注册自定义 getter：
  HTTP 仍为 200，指标使用缓存 299 和预期标签，自定义 getter 在插件读取时执行且看到正确 `_plugin_name`。
- 直接 C filter 配置中注入 Lua header callback，必须失败关闭且无后端命中；不能静默跳过插件。
- 原核心加载守卫、插件异常 500 后下一个请求恢复、上游不可用 502、客户端断连都有检查。
- C JSON 比较的是解码后完整对象，与独立的通用 table/cjson 路径做差分；包括引号、反斜线、换行、NUL、UTF-8、false/null、
  缺失字段、允许/拒绝/错误、小数、精度边界、毫秒转换及连续更新失效。不能通过删掉诊断变量赢得吞吐。
- 相同回归通过模块 ASan/UBSan。仅新模块 instrument，其他 DSO 未 instrument，leak detection 关闭。
  不据此声称长期无泄漏或生产稳定性优于原版；完整 APISIX Test::Nginx 在当前主机不可运行，未声称通过。

验证日志：`results/verification-optimized.log`、`results/verification-sanitizers.log`。

## 已解决的实施问题

这些问题已修复，最终验证通过，记录是为了避免后续重复踩坑：

- 用 `ngx_atofp(..., point=0)` 读取整数字段会得到无效值；已改用适合 off_t/整数的解析，补齐数值类型断言。
- 元数据缓存字段曾误放到候选路由结构而非请求结构，造成编译错误；已纠正，没有遗留待解决编译失败。
- header 名中的 `-` 与 NGINX 变量名中的 `_` 规范化曾导致缓存失效不完整；已分离变量键缓冲区并测试读→改→再读。
- 直接过滤阶段初稿虽能跑通，但曾额外进入 Lua VM；最终重构为纯 C filter，避免保留不必要入口。
- 直接覆盖旧 `.so` 会伤及仍 mmap 该 inode 的 worker；重建工具已改为原子 inode 替换。
- 不用每轮 Docker rebuild 保存大型 VFS 镜像：复用可丢弃 builder 容器，编译当前 `.so` 后复制到专用 demo 容器。

## 尚未尝试、值得另一个模型验证的方向

下面都是假设。没有为它们测得收益，也没有授权为了它们扩大插件/协议兼容范围。
先检查源码和现有第三方库是否已经做了对应优化，再决定是否值得实验。

| 优先级 | 假设与候选位置 | 验证办法与限制 |
| --- | --- | --- |
| P0 | 重新剖析最终构建：`plugins.c`、`request.c`、`merge.c`、`upstream.c` 的边界/调用次数仍可能偏多 | 补请求级调用/分配预算，分开 rewrite/access/balancer/log；最终吞吐必须换回无剖析构建 |
| P0 | JIT trace 被 C 函数返回/元方法打断，剩余开销可能来自频繁 C API 入口而非 C 算法 | 分析最终 trace abort/side exit、GC/解释比例；允许薄传输/缓存回放，不把核心决策重新放回 Lua |
| P1 | 稳定字段的重复字符串比较/变量解析可改为初始化期索引或预解析读取计划 | 研究 NGINX indexed variable API、已知字段枚举与现有 typed API；保留动态变量、自定义 getter、numeric capture、nil/false 和失效 |
| P1 | 限流诊断完整 JSON 编码仍逐请求执行，可能进一步按需物化 | 在 C 请求上下文保留结构化数据，NGINX/Lua 真正读取时编码；必须保留第三方 observer、显式赋值、JSON 内容和同请求更新顺序 |
| P1 | C 内 Lua table 字段访问、registry ref 与跨阶段元数据操作还可合并 | 统计 rawget/rawset/ref 次数，考虑 request 本地句柄或编译 slot；插件可见 table ABI、身份和代际生命周期保持 |
| P1 | 无 before_proxy 回调时，balancer hook/peer 适配可能仍有可省路径 | 检查 NGINX upstream peer 接口与现有 OpenResty 连接池耦合，先做调用预算；重试、失败回报、keepalive、timeout 不可省略 |
| P2 | 请求池临时对象和小字符串可复用/预分配，减少 malloc/GC 或 Lua intern | 先区分 NGINX pool 分配与 Lua GC；不可跨请求缓存请求池指针，不建立无界 worker 缓存 |
| P2 | 指标底层 SDK/第三方 label 处理或共享字典路径仍有重复工作 | 先核实第三方 Prometheus 是否已缓存；只优化底层等价机制，不能改 Lua Prometheus 业务、关指标或改变 404/401/429 计数 |
| P2 | 编译器优化/链接布局可能有增益 | 固定依赖下试 O3/LTO 等并记录体积和回归；仍不绑定私有 ABI，不用 instrumentation 构建验收 |
| 方法 | 云机器波动和先 Lua 后 C 的固定顺序可能影响小差距 | 不增加单样本时长；考虑交替次序、记录 CPU 时间/频率/调度信息；单样本不宣称显著性 |

不要把提高连接数/改后端响应/减少抓取频率等负载变化直接记为核心实现收益。
不要关闭日志/指标、换低成本失败响应、降低认证/配额语义、跳过清理、禁用错误处理来提高数字。
对于每个候选，应明确“假设→代码证据→最小修改→正确性差分→一分钟配对测量→保留或回退”。
若要讨论约 40%–50% 是否可达，先估计当前 worker 中仍可优化的核心成本与不可改的插件/传输成本；不能从语言名称推导上限。

## 复现与继续工作

先切到本分支并读 README 的固定镜像构建步骤。以下命令从仓库根目录运行：

```sh
python3 experimental/native-http-core/tools/dev.py
APISIX_CORE_SANITIZER=address,undefined python3 experimental/native-http-core/tools/dev.py
python3 experimental/native-http-core/tools/dev.py
python3 experimental/native-http-core/tools/run.py start native
python3 experimental/native-http-core/tools/run.py test
python3 experimental/native-http-core/tools/run.py bench
python3 experimental/native-http-core/tools/report.py
```

默认 dev.py 是完整可运行回归入口；先构建 README 指定的镜像，不能假设仓库自带 OpenResty/wrk。
测量会覆盖 `results/` 的当前轮文件；提交新优化前应先完整归档当前轮，保留源码/二进制/环境一致性。
本次没有因为整理文档而重跑压力矩阵：模块、插件、测试和测量 harness 的字节与最终已验证版本一致。
运行中的 demo 已恢复 native：HTTP 9080、全局 Prometheus 9091，演示限流为每窗口 5 次。

原工作区 `/workspace/apisix` 的 master 保持干净；研究工作区 `/workspace/apisix-native-demo` 使用本分支。
这两个绝对路径是本次环境的信息，不是其他机器运行脚本的必需目录。
用户现已授权提交与推送本研究分支，先前设计文档中“暂不提交/推送”仅记录当时权限。
没有要求创建 PR、合并到 master 或发布服务版本。

## 本次一并保存的证据

- 新 demo、旧局部原型、测试/工具、结果报告及设计上下文。
- 两份 `results/` 下现有的原始测量、配置、依赖/哈希、验证日志、剖析输出和已有历史源码快照。
- 已有历史模块与最终模块的小型 `.so` 二进制，用来核对研究证据；它们只适用于记录的固定 Linux/运行时，不替代源码构建。
- `results/EVIDENCE_MANIFEST.json` 记录归档文件的 SHA-256 与大小，不把模块哈希等同于可复现构建保证。
- 原始配置和 profiler 输出保留生成时的空白字节，归档目录用 `.gitattributes` 仅豁免这些原始文件的空白格式检查。
- 不提交 Docker 文件系统、依赖缓存、私有凭据、宿主机配置或无关项目内容。

部分较早轮次没有完整源码快照（尤其初始两轮），不能仅凭其二进制/数字宣称可从该轮源码重新构建。
历史源码快照仅作为档案，不应被当作待合并的另一套生产实现；当前入口始终是本目录的 `module/` 与 `runtime.lua`。
