# 2026-10-31：继续穷举有依据的核心优化候选

这是上一轮 [单插件实验](EXPERIMENTS_20261031.md) 之后的继续研究。原报告与
`results/single-20261031/` 不改写、不覆盖。本轮新证据写入 `results/exhaustive-20261031/`。
“穷举”指下表列出的、现有源码和剖析支持的候选集；有限实验不能证明所有架构、编译器、
硬件及工作负载上的绝对性能极限。报告会分别标明尝试完成、测量负收益和语义上未采用的方向。

用户继续授权尝试此前怀疑的优化点，也允许底层 SDK/框架配合修改。业务插件仍不改写；
路由、合并、调度、请求状态和 cache admission 等核心决策仍由 C 负责。
Lua 框架中的少量 cache replay 仅回放 C 已发布的数据，并使用既有运行时 dirty bit。
不因这次允许框架协作而改变认证、限流共享配额、指标、yield、重试或 keepalive 语义。

## 固定比较口径

- 稳定版 APISIX 3.19.0，commit `473043623c161966f3f6ef4e8f12d2d8667a12d2`。
  Native 与 Lua 两边加载同一套未修改的该版本插件及辅助源码；`ai` 均不加载。
- 三个独立路由插件场景：Prometheus、key-auth、local limit-count。只有 Prometheus
  场景抓取指标；其他两场景没有全局指标插件。
- 单样本仍为5秒预热、60秒压力。有效认证 key、大配额、真实代理、相同连接和响应体。
- 共同固定运行时沿用上一轮；稳定版源码搭配历史固定依赖，不是原装发行镜像比较。
- 新 Native 候选以旧轮选择的 `apisix_core_lazy_rate_json on;` 为控制条件，除非具体
  样本环境文件另行注明。旧轮无收益的 log_var_plan、fused_balancer 不默认混入候选。
- 不把诊断构建的 QPS 用作验收，不把 instrumented self 或调用次数当作整个 worker CPU 占比。
  每个样本保留配置、源码/模块哈希、原始 wrk、错误和父级 CPU 节流统计。

## 修改前热点排序：结构预算与测量边界

这里按可消除的重复工作、适用范围和语义风险排实验优先级，**不是 worker CPU 占比排名**。
代码链接定位本轮源码，函数名用于后续行号变化时复核。历史 `single-20261031` 的函数剖析
来自开关全部关闭的构建；本轮控制路径另启用 lazy JSON，不能把旧函数 self 比例当成当前值。
没有 key-auth 的同类函数剖析，下面的认证方向来自源码结构，不能冒充认证的 CPU 实测。

| 优先级 | 已确认的重复工作与源码 | 证据、候选及尚未确认的部分 |
| --- | --- | --- |
| 1 | [http.c:ac_var_assign/ac_read_var](module/http.c#L44) 与 [upstream.c:ac_native_var_write_fast](module/upstream.c#L612) | 旧单插件每请求18次赋值、8次 HTTP 读取、9/10次 native setter；`batch_vars`、`http_var_plan`、`native_var_writes` 针对其中重复取表、名称处理和 value 对象。存在重复工作已确认，可节省的 CPU 尚未单独归因 |
| 2 | [module.c:ac_request/ac_cleanup](module/module.c#L181)、[routes.c:ac_match_start/ac_accept_route](module/routes.c#L572)、[http.c:ac_only_decoded_or_merged/ac_http_prepare](module/http.c#L185) | 静态路径逐请求创建 api_ctx、opts/matched、候选数组、metadata-match 数组和普通 URI 临时缓冲；对应 request_pool、lazy_route_opts、route_candidates、lazy_meta_matches、plain_uri。命中路由时 matched 仍需创建，lazy opts 只少一张表及一个 registry ref；未命中无 filter 可少两张表。池操作本身也增加 Lua 调用/清表，净收益待测 |
| 3 | [plugins.c:ac_next/ac_return_call/ac_complete](module/plugins.c#L546)、[merge.c:ac_route_context_at](module/merge.c#L263) | 旧样本约3次上下文发布、10次 ac_request、3次 ac_next/请求；phase_context 每个成功普通回调少3次 registry rawget，phase_index 跳过不含当前回调的调用项。单插件列表很短，不能从次数推导显著收益；配置编译还增加索引数组 |
| 4 | [request.c:ac_headers_for_context/ac_header/ac_modify_header](module/request.c#L312)、[runtime.lua:cached_headers/cached_header](runtime.lua#L47) | key-auth 源码通过 core.request.header 读密钥，SDK 热命中仍有 C/Lua 跳转，dirty wrapper 和临时 header key 仍有成本；对应 sdk_cache_replay、request_header_flags、header_scratch。这里是可定位的边界假设，没有每次边界周期或认证专属 CPU 分解 |
| 5 | [runtime.lua:M.balance](runtime.lua#L93)、[tools/rebuild.py](tools/rebuild.py#L20) 及原生路由算法 | balancer 融合上轮已尝试并保留负结果，不能重新包装成新发现；O3/LTO 是布局/编译候选。路由算法已在 C 中，按下文独立成本门槛决定是否继续算法实验 |

归档的 [Prometheus 函数日志](results/single-20261031/profile-native-prometheus-functions.log) 和
[limit-count 函数日志](results/single-20261031/profile-native-limit-functions.log) 可复核以下计数；
分母分别为138,449与148,507次 `ac_http_prepare`，窗口边缘的 cleanup/filter 会造成小数误差：

| 原 Native off 的调用数/请求 | Prometheus | limit-count |
| --- | ---: | ---: |
| `ac_push`（registry rawgeti） | 77.002 | 71.000 |
| `ac_request`（查上下文，不等于创建对象） | 10.000 | 10.000 |
| `ac_var_assign` | 18 | 18 |
| `ac_read_var` | 8 | 8 |
| `ac_native_var_write` | 9 | 10 |
| `ac_route_context` | 3.000 | 3.000 |
| `ac_match_start` | 1 | 1 |

普通成功、无重试/脚本/filter 的这两个场景，薄 Lua 帧仍有2次 `start_phase`、1次 `step`、
1次 `balance_start`、1次 `balance_finish`，共5次显式 engine 入口；插件 SDK 边界另外计算。
`ac_request` 计数含同一 C 帧内的重复 helper 查询，不能当作 C/Lua 往返数；`ac_push` 不创建表，
NGINX pool 分配也不等于 malloc 或 Lua GC 分配。上轮已经完成的桥接去表、配置/执行计划缓存、
直接 runtime C API、阶段融合、元数据缓存、C filters、C JSON 编码及日志预取，不再算成新候选。

既有短诊断窗口约10秒，仅用于发现方向，不是60秒压力样本：

- [Native Prometheus JIT 原始输出](results/single-20261031/profile-native-prometheus-jit.raw)：
  C81%、compiled8%、interpreted4%、GC7%；对应 Lua 是C61%、compiled22%、interpreted9%、GC7%。
  `observe` 占 Native **GC 状态内部**样本的64%，不能写成 worker CPU 的64%。
- [原 Native limit trace hooks](results/single-20261031/profile-native-limit-traces.raw) 为0次 abort、179次 texit；
  [Prometheus hooks](results/single-20261031/profile-native-prometheus-traces.raw) 为7次 abort、1,095次 texit，
  abort 均为 `NYI: return to lower frame`。这些 hooks 不统计全部边界，也不计每次 exit 的耗时，
  没有依据说持续大量 trace abort 已被确认为首要瓶颈。
- 上轮最终采用的 [lazy JSON limit JIT](results/single-20261031/profile-selected-limit-jit.raw) 为
  C91%、compiled3%、interpreted3%、GC3%；[hooks](results/single-20261031/profile-selected-limit-traces.raw)
  是0次 abort、185次 texit，heap快照约1,563–2,667 KiB。快照反映存活堆，不是分配速率；
  C状态含核心算法、NGINX/库工作和语言边界，不能把91%都算作可删除的桥接损耗。
- [function_sampler.c](tools/function_sampler.c#L20) 使用 `CLOCK_MONOTONIC_RAW` 墙钟，self仅扣除
  已 instrument 的子调用；未 instrument 的 Lua/库/下游 filter 和调度等待仍计入 self。
  `ac_var_assign` 的5.5%/5.8%、`ac_push` 的4.8%/4.3%只说明值得调查；不能相加为预期收益。
  `ac_native_body` 的25.4%/18.9%包含 `next_body`，也不能当成 wrapper 自身成本。

本轮仍缺无扰动的完整 native 调用栈、每条 C/Lua 边界的周期，以及按对象类型/分配站点归因的速率。
后文已补充公开 Lua allocator 的总调用/申请字节诊断；它不提供上述站点归因。
上述排序先由结构及旧诊断建立，再由可回退开关与配对吞吐验证；结果不能反向改写成事先已证明的 CPU 排名。

## 首批13个运行时开关

所有新开关默认关闭，可单独消融，也可组成组合候选；[module.c](module/module.c#L32) 注册它们。
关闭开关保留控制算法，但共享 helper 和已发现的正确性修复仍在，因此不是上一轮原二进制。
下表保留首批13项；追加 `request_pool_native`、`matched_pool`、`balancer_peer_plan` 后，
本轮候选集共16个运行时开关，另有O3、O3-lto、O3-lto-local三个构建变体。
追加项见后文，不能与首批筛选快照混为同一构建。
下表是实现和语义清单，具体测量与最终选择见后续结果区。

| 开关（均有 `apisix_core_` 前缀） | 源码位置与实际修改 | 语义约束与验证重点 |
| --- | --- | --- |
| `batch_vars` | [http.c:ac_var_batch_begin/assign/end](module/http.c#L61)，调用点为 ac_http_prepare/ac_forwarded 与 [upstream.c](module/upstream.c#L282)：连续赋值复用栈上 cache 句柄，少做 registry → api_ctx → var → cache 查找 | 仅覆盖连续无 Lua 回调的 C 写入段；raw var/cache 缺失或 cache 有 metatable 时走逐次原路径；保留 forwarded 原始值与 getter 的读取/赋值顺序；不跨 getter/yield 保留栈索引 |
| `http_var_plan` | [http.c:ac_http_read_plan/ac_read_var](module/http.c#L98)：11种固定名称预存长度，配置期预算 hash；普通样本实际执行8次读取 | 仍逐次检查自定义 getter，保留 named lookup，不增加变量索引或改变 cacheable/no_cacheable 行为 |
| `plain_uri` | [http.c:ac_only_decoded_or_merged](module/http.c#L185) 和 [ac_http_prepare](module/http.c#L342)：普通路径免解码缓冲，无参数 request_uri 免拼接缓冲 | 只有 path 等于 NGINX URI 且无百分号时跳过解码；无参数时借用只读字节，Lua 字符串仍复制；保留 servlet/tail/encoded-slash 分支 |
| `lazy_route_opts` | [routes.c:ac_match_start/ac_accept_route](module/routes.c#L572)：候选均无 Lua filter 时不建 opts、不持有其 registry ref；命中后才新建 matched | 仍在原时机读取 remote_addr；matched 保持每请求独立且插件可见；存在任一候选 filter 时保留原 opts 路径，包括未选中 filter 的情况 |
| `route_candidates` | [routes.c:ac_collect_candidates/ac_match_start](module/routes.c#L545)：首个实际候选才分配一个 slot，0/1候选不排序 | 多候选维持原数组扩容、优先级/host/sequence比较器；单 slot 策略可能增加多候选扩容成本 |
| `request_pool` | [module.c:ac_request/ac_cleanup](module/module.c#L181)：外层 api_ctx 改用 tablepool 的 native_api_ctx 池，沿用32 hash-slot初始容量 | 只回收 Lua 表，不回收 C request/pool 指针；完成 log 且 request cleanup 才释放，有插件 metatable 不回收，未完成 log 的异常路径保留 GC；保留活跃请求/yield 身份；结束后别名限制见下文 |
| `lazy_meta_matches` | [module.c:ac_request](module/module.c#L253) 与 [plugins.c:ac_enabled](module/plugins.c#L419)：从逐请求预分配4个 filter-match slot 改成首次缓存表达式结果时初始化 | 没有 filter 时不分配；仍缓存同请求表达式结果，保留配置/请求池生命周期 |
| `phase_index` | [plugins.c:ac_compile_plan/ac_plan_has_phase/ac_next](module/plugins.c#L44)：配置 calls 排序完成后，为各阶段保存现有 ac_call_t 指针 | 不提前决定 skip/filter/prefer-route/consumer rewrite；consumer_only 后续写原对象仍可见，索引不另持有 Lua ref；配置和临时 consumer plan 原释放路径保留 |
| `phase_context` | [plugins.c:ac_return_call/ac_complete](module/plugins.c#L546)、[merge.c:ac_route_context_at](module/merge.c#L263)：同一同步片段复用栈 table 句柄 | 每个成功普通回调的准备少2次、完成少1次 ac_push；元数据和 _plugin_name 仍按原顺序写入/恢复，pre/error/script 分支保持原行为，不跨 callback/yield 保存栈索引 |
| `request_header_flags` | [request.c:ac_request_headers_changed/clear/ac_headers_for_context](module/request.c#L284)：非空 request 直接调用既有 runtime wrapper 使用的导出 dirty-bit C 原语 | 未取得 request 时保留 wrapper；源码没有额外 fd/fake-request 判断，不声称该条件已回退；必须覆盖外部 ngx.req.set_header 与 SDK 写入交错 |
| `header_scratch` | [request.c:ac_modify_header](module/request.c#L388)：header/cache key 名称长度≤122时使用128字节栈缓冲，长名称仍用请求池 | Lua 字符串复制字节；NGINX header 名和值仍走原所有权路径，不能将临时缓冲地址留给后续阶段 |
| `sdk_cache_replay` | [runtime.lua:cached_headers/cached_header](runtime.lua#L47) 与 [request.c:ac_header/ac_request_open](module/request.c#L358)：raw cache 命中留在 Lua，miss 直接进 C helper，少建临时 C closure/嵌套 VM 调用 | dirty bit 未置位才回放；raw miss 回原路径，避免元方法执行两次；首元素保持 Lua的 table[1]及and/or语义（含 false/nil、__index），不是无条件 rawgeti；nil/false ctx仍取当前请求 |
| `native_var_writes` | [upstream.c:ac_native_var_write_fast](module/upstream.c#L612) 与 [http.c:ac_native_var_write](module/http.c#L11)：用栈上 variable value 调用本模块 setter，省通用 setter 的小写缓冲写入及 value 对象池分配 | 仍有大小写检查/hash/NGINX查找；仅最终 handler 是 ac_var_set 且 CHANGEABLE 时使用，foreign/大写/无请求/假请求回退；字符串仍复制入请求池，不新增 index、不改变 slot 缓存 |

本轮 header 检查还修正了原 Native 对空数组、首元素 false/nil 和数组 __index 的读取差异，
对齐稳定 Lua 的 `type(value) == "table" and value[1] or value`。这是开关两态共享的正确性修复，
不能归因成 sdk_cache_replay 的独立收益。原 Lua 的带横线 header 写入与变量 cache key 差异，
仍在专项中显式记录为已有差别，不以修复它来虚增本轮性能收益。

## 已核实、排除重复提出的第三方优化

此次重新只读检查了上轮下载源码及 hash；没有把依赖内部已有缓存列为新收益来源。
Prometheus 依赖仍是固定 `nginx-lua-prometheus-api7-1.1.0-1`，版本与稳定版 rockspec 的差异见固定口径。

| 已有机制和证据位置 | 对本轮实验的约束 |
| --- | --- |
| `prometheus.lua:381–457 lookup_or_create` 已按 label 值树缓存完整指标名，并在396–401行避免 ipairs 以形成较长 trace | “增加标签/完整名称缓存”不是新的未尝试点。`observe:594–635` 已逆序遍历桶并在满足条件后停止，不能把已有处理重复计功 |
| `prometheus_resty_counter.lua:26–33,96–104` 已有 worker 本地 increments 和 timer sync | 指标并非每个增量都访问共享字典；不能以再次本地累加作为发现。该版本 incr 仍创建 `{v=step,t=exptime}`，属于可见分配但当前 CPU/GC贡献未量化；本轮不改第三方指标逻辑 |
| [ngxvar v0.5.2 wrapper](results/single-20261031/audit/native-ngxvar-http-v052.lua) 已对 uri/host/地址/scheme/timing 使用 typed C API，模块 request.c 的 typed fetch 已复用这些导出原语 | 直接 typed API 和原生时间变量读取已经做过；request_length/bytes_sent 的通用路径仍需保留各自缓存语义，不等于还有一套未使用的全新缓存 |
| [NGINX 1.27.1 ngx_http_get_variable](results/single-20261031/audit/native-nginx-variables.c#L678) 对已有 INDEXED 名称直接调用 get_flushed_variable | 名称预解析只省名称处理；不能一律新增 index 或改用未刷新 indexed get。上轮 bytes_sent 提前读后冻结的失败日志保留，http_var_plan 因此只预算长度/hash |
| [request.c:ac_set_vars/ac_release_vars](module/request.c#L964) 已调用 tablepool 和 table.clear；稳定版 core/ctx.lua:447–465也复用 ctx_var/cache | ctx.var 和 _cache 复用已经存在。本轮 request_pool 只补 Native 外层 api_ctx；稳定 Lua init.lua:853、1581已 fetch/release 外层表 |
| [sdk.c:ac_require](module/sdk.c#L5) 已先查 registry `_LOADED`；[runtime.lua:cached_index](runtime.lua#L30) 已回放 C 发布的变量 cache | 不是每次重新加载模块，也不能把普通变量 cache replay 再算作新点；本轮 sdk_cache_replay 只延伸到 header SDK。永久缓存可变模块成员必须另建失效契约 |

Prometheus rock、prometheus.lua、prometheus_resty_counter.lua 的复核 SHA-256 分别为
`f9b867c3926c6bdd4d5303b54ce8266b0e77dafa6bd1bd3833c14d77b6b68ceb`、
`431906affc135bcdf632c80da936088d15486ff0b0b207a0c29c1deb67e8d01d`、
`749ecb10c6dd2060dbf081c93005a87f757679ff2eb81c99989a2553bd96bb01`，与
[上轮审查](EXPERIMENTS_20261031.md#已核实的第三方优化)一致；ngxvar/NGINX来源及hash见
[audit/sources.json](results/single-20261031/audit/sources.json)。

## 编译器候选及路由成本门槛

`APISIX_CORE_OPTIMIZATION` 支持 `O3`、`O3-lto`、`O3-lto-local` 三个构建变体，
默认仍是 `O2`。最后一个在 `-O3 -flto` 上增加 `-fno-semantic-interposition`，
允许本模块自身定义的函数调用按本地绑定优化；外部NGINX/LuaJIT符号仍按原运行时绑定，
不使用私有ABI，也不声称支持对本模块内部调用的任意符号替换。
不启用 fast-math、不更改运行时 ABI、不用减少校验或容错来换编译收益。
每次重建记录编译器、选项、模块 SHA；sanitizer/函数剖析/路由诊断构建不能参与验收。
各变体通过相应正确性检查后才测量；若组合或代码布局影响结果，按整个构建报告，
不把总收益分摊成精确单项贡献。

路由树、候选优先级、host/method/CIDR、参数和表达式已经是 C 实现。
本轮先测路由本身的 CPU 成本，**5%门槛排除 Lua table/registry 兼容层成本**：

- `collect_sort`：候选收集/数组/排序；`path_match`：路径及参数匹配。
- `eligibility_expr`：包含表达式，有自定义 getter 时可能进入 Lua，必须注明这一混合成本，
  不能直接并入“纯 C 路由算法”。
- `options`、`accept_context`：包含 Lua ABI/table/metadata 适配，单独报告，不用于算法门槛。
- `AC_ROUTE_PROFILE` 使用线程 CPU 时钟，并记录空区间校准值；校准只近似扣除测量税，
  会扰动执行，不是无偏的生产 CPU 分布。
- 如果排除兼容层后路由算法不足5%，本场景不再投入一次架构重写；这不能说明大路由表、
  更复杂表达式或不同硬件上同样不足5%。若超过门槛，再根据具体子项决定算法实验。

## 仍须保留的动态行为

不能以“已经是 C”或调用次数多为由直接删除以下操作：

1. callback 后元数据恢复。Lua 插件可写 `ctx.conf_type/id/version`；仅缓存“上次已写”会
   留下被污染的值。`phase_context` 只复用 table 句柄，仍执行全部写入。
2. `_skip_plugins` 每个实际 callback 的查找。workflow 或插件可以在此前 callback 中修改表，
   未建立原生写入协议时不能跨 callback 缓存结果。
3. `_plugin_name` 的设置和清除，以及 getter 的读取时机。移到别的阶段会改变指标标签或用户逻辑。
4. log 清理、上游反馈、代际 release 与请求结束清理。没有 log 插件也不等于没有核心工作。
5. 模块加载已通过 registry `_LOADED` 缓存；进一步永久缓存可变模块成员需要清晰的失效契约。
6. worker 全局 current-request 指针不能替代合法 coroutine 上下文，yield 后不同请求会交错执行。
7. `request_pool`、`request_pool_native`、`matched_pool` 采用原版 Lua 的请求结束生命周期：
   插件在请求结束后继续持有 api_ctx 或 matched，可能看到被清空/复用的表。稳定Lua已有
   api_ctx和matched_route_record池（init.lua:1575、1581），这不保留旧Native不复用时的
   “已结束请求 table 永久独立”特性，不能声称所有 escaped alias 均不变。活跃/挂起请求仍独占，
   只在log完成后的NGINX cleanup归还；同请求重新accept的旧matched只unref，不立即入池。
   cleanup发现curr_req_matched已被插件替换时，既不清空原表，也不把替换表放进池；metatable表同样不归还。

SDK依赖函数替换还有明确边界：[runtime.lua:cached_headers/cached_header](runtime.lua#L47)
在构造wrapper时捕获 `resty.apisix.request.is_request_header_set` 函数；稳定Lua的
core/request.lua捕获模块表后，每次仍读取该成员，Native off也逐次读取。
因此初始化后monkeypatch这个成员，replay热命中不会跟随替换；`request_header_flags` 在取得request时
更直接调用固定runtime导出的C原语。这两个开关按固定runtime及当前未修改插件工作负载验证，
**不声称对任意SDK函数热替换完全等价**；自定义变量getter的注册/读取时机是另一套已保留的机制。
C引用池则每次从 `_LOADED` 读取当前table.clear并受保护调用，不永久捕获它。

健康检查、慢启动、服务发现及范围外协议继续不扩展。业务插件和第三方指标业务逻辑不通过
重写、删字段、关指标、快速失败或配额本地化来优化。

## 候选集冻结后的剩余方向分类

本轮在首批13项和追加3项之外完成一次源码完整性审查，之后冻结候选集；除具体正确性缺陷外，
不继续增加没有独立热点依据的开关。这是可解释的研究停止范围，不是绝对性能极限。

| 方向 | 分类与代码依据 |
| --- | --- |
| matched/opts对象 | matched残余分配已纳入第15项。无filter热路径的opts已由lazy_route_opts消除；[routes.c:ac_match_next](module/routes.c#L723) 的filter路径会把opts传给Lua并可yield，只为该慢路径再建池没有当前单插件热路径证据 |
| 空rewrite/access/before_proxy | 不能直接跳过：[plugins.c:ac_advance](module/plugins.c#L443) 做consumer merge；[ac_next](module/plugins.c#L577) 做DNS/选点/上游准备；[ac_leave](module/plugins.c#L731) 设置代理响应标记。phase_index已处理缺少callback的列表，不能把核心工作当作空阶段 |
| 重复script检查 | [script.c:ac_script_next](module/script.c#L54) 读取可见route.value.script；跨callback永久缓存会忽略对route表或其元方法的动态改变。没有新的配置不可变契约时，不以删读取保持“完全等价”的名义优化 |
| 无log插件时移除Lua入口 | [plugins.c:ac_leave](module/plugins.c#L731) 仍有上游反馈、release_vars/tracer、ngx_ctx解绑。真正省掉入口需另建NGINX log handler，并证明Lua cleanup上下文、错误传播和热更新条件；不是删除一个空函数的小实验 |
| C空helper与同帧上下文查询 | [native_phases.c:ac_native_body](module/native_phases.c#L54) 的无tracing helper以及同一同步C帧中的ac_request重查，不新增Lua往返。独立CPU贡献未测，本轮纳入现有O3/LTO编译考察，不继续逐个拆开关；不声称编译器已消除这些调用 |
| balancer剩余边界 | 首次入口融合是上轮已有负结果；第16项只省静态地址格式化/CRC。跳过整个balancer还要接管ngx_lua peer、timeout、retry和keepalive耦合；[upstream.c:ac_balance_finish_ctx](module/upstream.c#L789) 中这些公开runtime调用全部保留，不使用私有ABI绕开 |

## 结论能覆盖到哪里

本轮可以完成并报告的是：这16个有源码依据的开关、选定组合及O3/LTO，在记录的固定硬件、
运行时、路由表和单插件负载下的正确性与60秒配对吞吐。单独无收益不证明任意组合无收益，
组合收益也不能无条件分摊给各个开关；结果表须区分未运行、失败、负收益和可保留候选。

30%–40%的吞吐目标，在相同CPU瓶颈且其他因素不变时，对应每请求成本减少约23.1%–28.6%。
当前结构计数和带扰动剖析没有证明还存在这么多可删除的核心成本。若有限候选没有达到目标，
应写成“这些候选在此条件下未达到目标”，不能写成“Native/C已达绝对极限”。早期省略完整职责的
40%–50%原型也不能作为完整实现的性能上限或必达值。

仍需区别测试分辨率与实现上限：单个60秒样本不是统计显著性；探索中挑出的最佳组合需要
独立交错配对确认，CPU节流/运行顺序和共同依赖版本也影响适用范围。超出当前候选集的
按分配站点/对象类型归因、完整调用栈、更大路由表、不同并发或硬件属于后续问题；不以删指标、缩减配额语义、
跳过可观察阶段或私有运行时ABI来宣称已在本轮语义约束内获得收益。

## 正确性与结果状态

运行时验证由主流程串行执行，避免与压力样本竞争。
新增专项包括 HTTP plans、route allocations、request pool、phase context、header fast；
原有六配置核心回归、真实插件差分和 ASan/UBSan 继续作为语义门槛。

`test_phase_context.py` 首次运行因生成模板含两个 log hook，而测试只允许一个而停止。
这是测试定位失败，未进入该专项的 HTTP 断言。修复为对每个 hook 注入同一个 URI guard；
off/on 两态的24个并发请求、合法 yield、完整阶段顺序和元数据污染恢复已通过。
失败日志保留，修复日志为 `verify-o2-phase_context-fixed.log`。

第一批13项全开通过 ASan/UBSan 下的六配置核心回归、demo、五个新增专项，以及稳定版
单插件开/关差分。日志为 `results/exhaustive-20261031/verify-asan-*.log`。
ASan只覆盖本模块，运行时DSO未整体插桩；泄漏检查关闭，与上一轮限制一致。

Header专项曾因测试假设错误失败：稳定Lua的连字符header写入清除 `http_x-probe`，读取
却使用 `http_x_probe`；旧Native已经规范化。共同缓存失效用例改用无连字符的 `Probe`，
另设断言明确保留两边历史差异。另一个首元素nil/false差异已修正C以匹配Lua的
`type(value) == 'table' and value[1] or value`，没有通过放宽测试绕过问题。
原失败记录与修复后 `verify-o2-header-fast-oracle-2.log` 均保留。

## 路由5%门槛的实测

原始证据是 `route-profile-metrics.raw` 与 `route-profile-auth.raw`；诊断窗口10秒、
负载15秒，只作CPU成本诊断，不作吞吐验收。正式压力样本仍全部60秒。

| 单插件 | 算法原始计时 / worker线程CPU | 近似扣空计时后 | options + accept_context，校正后 |
| --- | ---: | ---: | ---: |
| Prometheus | 1.139% | 0.421% | 3.043% |
| key-auth | 1.473% | 0.601% | 4.012% |

算法列是 `collect_sort + eligibility_expr + path_match`。这两个场景无route表达式、
filter、regex或多候选；即使不扣计时税也远低于5%，因此不替换已有C压缩前缀树算法。
分母同样包含诊断税，校准不是严格上界；结论只限当前单普通前缀路由，不能推广到大路由表。
兼容对象准备成本单列，不能把它归给匹配算法，也不能宣称整个路由子系统不足5%。

后续审查还发现旧SDK中的 `ctx.headers=false` 等truthiness差异；筛选源码与二进制先冻结，
修复必须独立记录并重新验证，不能将共享正确性修复计为单开关性能收益。

## 筛选方法

13项采用固定顺序单样本筛选，10个通用项测Prometheus、3个header项测key-auth。
每3项前后插入一个Native控制样本；每项相对共享两侧对照QPS均值计算变化。
这有顺序、多重选择与共用对照的偏差，仅用于选组合，不能作为独立因果效应或显著性检验。
所有负收益、测试失败和修复均留档；独立最终配对用于回答稳定Lua对Native的比较。

`tools/analyze_sweep.py results/exhaustive-20261031` 可从原始JSON重新计算表格。
筛选的源码及二进制保存为 `screen-module-source/`、`screen-runtime.lua`、`screen-module.so`。

## 13项冻结筛选结果

相对共享前后控制的均值，全部为单个60秒压力样本；数值不代表统计显著性。

| 实验 | 场景 | Native QPS | 前控制 | 后控制 | 变化 |
| --- | --- | ---: | ---: | ---: | ---: |
| `batch_vars` | metrics | 20219.26 | 19543.74 | 19889.97 | +2.55% |
| `http_var_plan` | metrics | 20003.68 | 19543.74 | 19889.97 | +1.45% |
| `lazy_route_opts` | metrics | 20372.69 | 19543.74 | 19889.97 | +3.33% |
| `route_candidates` | metrics | 20137.06 | 19889.97 | 20370.00 | +0.04% |
| `request_pool` | metrics | 20387.22 | 19889.97 | 20370.00 | +1.28% |
| `lazy_meta_matches` | metrics | 19794.21 | 19889.97 | 20370.00 | -1.67% |
| `phase_index` | metrics | 20069.92 | 20370.00 | 20267.92 | -1.23% |
| `phase_context` | metrics | 20090.80 | 20370.00 | 20267.92 | -1.12% |
| `plain_uri` | metrics | 19750.76 | 20370.00 | 20267.92 | -2.80% |
| `native_var_writes` | metrics | 19848.50 | 20267.92 | 19691.81 | -0.66% |
| `request_header_flags` | auth | 24482.56 | 23393.09 | 24018.46 | +3.28% |
| `header_scratch` | auth | 24518.56 | 23393.09 | 24018.46 | +3.43% |
| `sdk_cache_replay` | auth | 23966.34 | 23393.09 | 24018.46 | +1.10% |

首批20个样本未发生失败响应或worker重启。header dirty-bit样本记录到1次父级CPU节流，
其余19个为0；各原始JSON保留完整计数。通用控制从19.5k至20.4k波动，
小于几个百分点的单项变化尤其不能当成稳定因果收益。优化组合需另行测量，不可把这些百分比相加。

## 三项追加实验与兼容性修复

追加三项后共16个运行时实验，全部默认关闭；首批13项筛选数据和二进制快照保持原样。

| 编号与开关 | 实现位置、可消除成本及语义边界 |
| --- | --- |
| 14：`request_pool_native` | [module.c:ac_table_pool_fetch/return](module/module.c#L195) 在C中维护有界registry引用池，既复用外层api_ctx表，也复用它的ref；[ac_request](module/module.c#L353) 在native池开关打开时优先使用它，否则保留首批tablepool实验/新建路径。区别于首批request_pool，不通过Lua tablepool fetch/release搬运池状态 |
| 15：`matched_pool` | [routes.c:ac_accept_route](module/routes.c#L663) 仅在match_opts_ref缺席时借用独立matched表；[module.c:ac_cleanup](module/module.c#L269) 用独立matched_ref核对当前可见表仍是模块原对象，先于api_ctx归还。它需要lazy_route_opts消除opts，因此消融控制也必须打开lazy_route_opts；filter拥有的matched沿用原路径 |
| 16：`balancer_peer_plan` | [upstream.c:ac_peer_plan_compile](module/upstream.c#L117) 在配置pool保存静态IP、sockaddr字节快照、长度、port、scheme及原snprintf/CRC算法的pool key；[地址校验](module/upstream.c#L151)、[peer_get](module/upstream.c#L570) 和 [balance_finish](module/upstream.c#L789) 每次检查当前selected节点，后者额外比较当前scheme内容。任一不符走原格式化/CRC路径 |

两个C引用池使用cycle共享状态，避免配置generation复制池计数导致重复借出；容量200，每20000次
归还清退。复用Lua表和registry引用，不复用NGINX请求指针。缺少table.clear、函数抛错、池满、
对象metatable或身份被替换时不强行入池；池的清理函数每次通过公开Lua API受保护调用。
请求结束后的alias限制与原版Lua生命周期一致，活跃请求、yield和同请求旧matched别名不能提前归还。

peer plan的配置内存由 [snapshot acquire/release](module/snapshot.c#L97) 固定到请求cleanup；
不是worker缓存中保存请求池地址。DNS节点编译时没有sockaddr，不建plan；
[ac_upstream_set](module/upstream.c#L743) 在请求级scratch配置中关闭plan，也保留原路径。
含DNS的请求副本若复制了静态节点，其只读plan仍由该请求固定的generation拥有。
重试仍针对当次selected节点重新校验，timeout/more_tries/recreate/current_peer/keepalive调用全部保留。
这与上轮“融合balancer首次边界”是两个独立实验。

新增 [test_peer_plan.py](tests/test_peer_plan.py) 首次在OFF态IPv6请求得到500，
[原失败日志](results/exhaustive-20261031/verify-next-o2-peer_plan-failed.log) 与
[error.log](results/exhaustive-20261031/verify-next-o2-peer_plan-failed-error.log) 显示
`failed to set native current peer: invalid port`。宿主支持IPv6，实际缺陷是旧Native向runtime的
URL地址解析接口传裸 `::1`。共享修复只在 [FFI调用前](module/upstream.c#L833) 对AF_INET6加方括号，
仍独立传port，balancer_ip和pool key的原始IP字节不变；此修复不计为peer-plan优化收益。

[修复后O2专项](results/exhaustive-20261031/verify-next-o2-peer_plan-ipv6-fixed.log) 使用稳定源，
OFF/ON均通过静态IP、动态scheme回退/内容相等、请求级upstream.set回退、DNS、失败重试、
keepalive pool身份及在途请求reload；两态均明确记录 `IPv6=PASS`，没有用SKIP掩盖失败。
[构建日志](results/exhaustive-20261031/verify-next-o2-peer_plan-ipv6-build.log)、
[构建flags和源码/模块hash](results/exhaustive-20261031/verify-next-o2-peer_plan-ipv6-build.json) 与
[修复后error.log](results/exhaustive-20261031/verify-next-o2-peer_plan-ipv6-fixed-error.log) 一并保留；
其中连接拒绝是用于触发retry的刻意输入，不是吞吐样本错误。

独立修复旧SDK的false/nil差异：headers/URI args/post args未命中、可选ctx、变量cache初始化、
IP/scheme/host回退、body参数、负max_size及JSON wrapper的false缓存返回。通用parsed body中的false
仍是合法缓存。修复已应用在后续共同二进制，不计为任何开关的独立收益；首批筛选快照保持原样。
SDK false检查的 [O2稳定源日志](results/exhaustive-20261031/verify-next-o2-sdk_false.log) 与
[ASan/UBSan日志](results/exhaustive-20261031/verify-post-asan-sdk_false.log) 已归档并通过。

最终16项全开构建已完成模块ASan/UBSan回归：六配置核心检查、真实插件差分、专项和稳定源
单插件开/关检查，证据为本轮 `verify-post-asan-*` 日志；构建及flags见
[verify-post-asan-build-build.json](results/exhaustive-20261031/verify-post-asan-build-build.json)。
[peer专项](results/exhaustive-20261031/verify-post-asan-peer_plan.log) 两态IPv6均通过；
[扩展池验证](results/exhaustive-20261031/verify-post-asan-pool-extended.log) 额外覆盖200容量、
20000次归还清退和并发yield。demo首次在抓指标时得到 `no data available`，
[失败日志](results/exhaustive-20261031/verify-post-asan-demo.log) 与
[重跑通过日志](results/exhaustive-20261031/verify-post-asan-demo-retry.log) 均保留。
ASan仍只覆盖本模块，第三方DSO未整体插桩，泄漏检查关闭，不能据此宣称长期无泄漏。

[稳定源O2单插件全开](results/exhaustive-20261031/post-o2-single-all.log) 也已通过；
[allocator lifecycle](results/exhaustive-20261031/allocator-lifecycle.log) 验证公开Lua分配器hook的
start/count/stop/restart、重复start拒绝、finalizer恢复及不显式stop时lua_close完成。
这是分配诊断工具生命周期验证，不是泄漏证明或吞吐结果。

## 追加三项筛选结果

以下仍为Native单插件Prometheus、每样本60秒，变化相对各自前后控制的QPS均值。
`request_pool_native` 和 `balancer_peer_plan` 的控制只开lazy JSON；`matched_pool`
及其两侧控制另开lazy_route_opts，不能拿另一组控制替代。worker CPU是整个worker的
CPU时间/完成请求数，不是该优化函数自身耗时。

| 实验与原始结果 | Native QPS | 前控制 | 后控制 | 变化 | worker CPU µs/请求 | 入选 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| [request_pool_native](results/exhaustive-20261031/extra-request_pool_native/results.json) | 20532.88 | 20260.52 | 19919.63 | +2.20% | 48.63 | 是 |
| [balancer_peer_plan](results/exhaustive-20261031/extra-balancer_peer_plan/results.json) | 20300.97 | 20260.52 | 19919.63 | +1.05% | 49.18 | 否 |
| [matched_pool](results/exhaustive-20261031/extra-matched_pool/results.json) | 20625.35 | 20402.76 | 20972.76 | -0.30% | 48.42 | 否 |

控制样本也完整保留，不能只展示三个候选：

| 控制与原始结果 | 配置 | Native QPS | worker CPU µs/请求 |
| --- | --- | ---: | ---: |
| [extra-control-0](results/exhaustive-20261031/extra-control-0/results.json) | lazy JSON | 20260.52 | 49.28 |
| [extra-control-2](results/exhaustive-20261031/extra-control-2/results.json) | lazy JSON | 19919.63 | 50.16 |
| [extra-matched-control-0](results/exhaustive-20261031/extra-matched-control-0/results.json) | lazy JSON + lazy_route_opts | 20402.76 | 48.95 |
| [extra-matched-control-1](results/exhaustive-20261031/extra-matched-control-1/results.json) | lazy JSON + lazy_route_opts | 20972.76 | 47.59 |

这七个样本的error_rate及父级nr_throttled均为0。matched_pool高于前控制、低于后控制，
保留其负变化；peer plan的+1.05%没有达到组合准入门槛，不能称为稳定收益。
request_pool_native的+2.20%只决定进入独立组合验证，不证明其效果具有统计显著性。

## 编译变体及O2控制

三种变体只开lazy JSON，均测单插件Prometheus；不是最终六开关组合的性能。
O3与O3-lto共享随后恢复O2测得的控制；O3-lto-local另有一次随后O2控制。
编译前的两个O2参考样本为上表extra-control-0和extra-control-2，较高者为20260.52 QPS。
下表“变化”只相对对应的后O2控制，不能与首批或追加实验百分比相加。

| 构建与原始结果 | Native QPS | worker CPU µs/请求 | 对应后O2 QPS | 相对后O2变化 | 入选 |
| --- | ---: | ---: | ---: | ---: | --- |
| [O3](results/exhaustive-20261031/compiler-O3/results.json) | 20438.51 | 48.89 | 19854.88 | +2.94% | 是 |
| [O3-lto](results/exhaustive-20261031/compiler-O3-lto/results.json) | 19908.05 | 50.18 | 19854.88 | +0.27% | 否 |
| [O2共享后控制](results/exhaustive-20261031/compiler-control-o2/results.json) | 19854.88 | 50.31 | — | — | 控制 |
| [O3-lto-local](results/exhaustive-20261031/compiler-O3-lto-local/results.json) | 19631.14 | 50.85 | 20834.60 | -5.78% | 否 |
| [O2独立后控制](results/exhaustive-20261031/compiler-local-control-o2/results.json) | 20834.60 | 47.95 | — | — | 控制 |

五个样本的error_rate及父级nr_throttled均为0。实际flags、源码/模块哈希见
[O3构建](results/exhaustive-20261031/build-O3-build.json)、
[O3-lto构建](results/exhaustive-20261031/build-O3-lto-build.json) 和
[O3-lto-local构建](results/exhaustive-20261031/build-O3-lto-local-build.json)。
编译优化未呈现单调提升；O3-lto-local的负结果及控制波动均保留。

## 选择规则与最终组合

[selection-rule.json](results/exhaustive-20261031/selection-rule.json) 在查看追加实验结果前
记录规则；[finalize_run.py](tools/finalize_run.py) 的执行条件明确为：运行时候选QPS必须
**高于前后两个控制，且相对两控制均值至少+2%**。不是要求比每个控制都高2%，
也不是统计显著性检验。首批初选为batch_vars、lazy_route_opts、request_header_flags、
header_scratch，加上上一轮采用的lazy_rate_json；追加三项中仅request_pool_native达到门槛。

编译变体须高于编译前两个BASE控制及其对应的后O2控制，同时至少比该后O2控制高2%；
若多个合格，选观察QPS较高者进入独立验证，否则保留O2。
O3-lto-local沿用事先记录的同一门槛。按已归档数据，只有O3满足条件。
[selection-final.json](results/exhaustive-20261031/selection-final.json) 保存全部候选判定，
最终配置为O3及以下六个开关；所有其他实验开关关闭：

```nginx
apisix_core_lazy_rate_json on;
apisix_core_batch_vars on;
apisix_core_lazy_route_opts on;
apisix_core_request_header_flags on;
apisix_core_header_scratch on;
apisix_core_request_pool_native on;
```

[选定构建元数据](results/exhaustive-20261031/selected-build-build.json) 记录O3和模块哈希。
探索样本的筛选偏差、代码版本与布局变化、组合相互作用由下文独立配对进一步检验；
两轮结果仍不能消除这些不确定性，也不能证明未入选开关在所有组合下无用。

## 选定组合验证及保留失败

前述16项全开模块ASan/UBSan已完成；选定O3六开关组合也通过六配置核心检查：
[default](results/exhaustive-20261031/selected-core-default.log)、
[servlet](results/exhaustive-20261031/selected-core-servlet.log)、
[tail](results/exhaustive-20261031/selected-core-tail.log)、
[encoded](results/exhaustive-20261031/selected-core-encoded.log)、
[trusted](results/exhaustive-20261031/selected-core-trusted.log)、
[untrusted](results/exhaustive-20261031/selected-core-untrusted.log)。
[branch legacy demo](results/exhaustive-20261031/selected-demo-branch.log) 与
[stable single-plugin](results/exhaustive-20261031/selected-single.log) 均通过，
后者包含指标、认证、配额、双worker共享配额及keepalive。

O3-lto的[首次单插件验证](results/exhaustive-20261031/compiler-check-O3-lto.log)
在Lua自身的Prometheus场景先失败：status计数已达到预期，histogram/bandwidth尚未同时可见；
原[error.log](results/exhaustive-20261031/compiler-check-O3-lto-failed-error.log)保留。
[test_single.py](tests/test_single.py#L43) 改为等待完整既有条件同时满足，再执行原断言；
没有删除指标检查、放宽精确status计数或修改插件/导出器逻辑。
[重跑](results/exhaustive-20261031/compiler-check-O3-lto-retry.log)通过，
[O3](results/exhaustive-20261031/compiler-check-O3.log) 与
[O3-lto-local](results/exhaustive-20261031/compiler-check-O3-lto-local.log)也通过。
这是指标缓存刷新可见性的测试同步问题，不能归因为编译器造成Native回归。

[选定组合demo首次失败](results/exhaustive-20261031/selected-demo.log)同样发生在Lua侧，
但原因不同：历史fixture要求上游看到规范化后的`/plain/ok`，稳定3.19.0的Lua核心
转发原始`/plain/a/../ok`；research branch与Native采用前一种URI策略。
[原error.log](results/exhaustive-20261031/selected-demo-stable-failed-error.log)保留。
没有为通过测试而改变URI安全策略：legacy demo继续对照branch既有契约，stable single
验证本次普通路由、三个单插件的共同benchmark契约。**完整稳定发行版语义等价性并未成立**，
单插件比较不能消除该版本策略差异。[专门的原始URI差异验证](results/exhaustive-20261031/uri-policy-difference.log)已通过并保留两边实际响应：
`/metrics/a/../ok` 在稳定Lua中原样转发，在Native中转发为 `/metrics/ok`。

[README的sweep重现说明](README.md#latest-sweep-reproduction) 使用新的
`APISIX_CORE_SWEEP_OUT`目录：screen首批13项、extras追加3项、compilers的O3/O3-lto、
linkage的O3-lto-local，随后finalize_run自动保存选择、验证、运行两轮反向配对及诊断。
手动final须使用独立label和reverse，避免覆盖历史证据。
两轮稳定Lua配对已完成，完整结果与身份核对见下文；后续诊断另列，避免混入吞吐验收。


## 最终两轮稳定Lua配对

选定O3六开关组合对稳定APISIX 3.19.0源码，每个场景独立启用一个插件，双方排除ai。
**两轮QPS均值对应的提升分别为Prometheus +7.80%、key-auth +3.97%、local limit-count +6.81%，
均未达到30%–40%目标。** 以下保留两轮全部结果，不挑最佳一轮。

原始证据：[第一轮results](results/exhaustive-20261031/final-round-1/results.json)、
[第二轮results](results/exhaustive-20261031/final-round-2/results.json)、
[逐对计算](results/exhaustive-20261031/analysis.json) 和
[均值汇总](results/exhaustive-20261031/paired-summary.json)。
第一轮metrics/limit按Lua→Native，auth按Native→Lua；第二轮各场景反向。
每个实现仍为5秒预热、60秒压力，单worker、2个wrk线程、32个连接。

| 轮次 | 独立路由插件 | Lua QPS | Native QPS | Native变化 |
| --- | --- | ---: | ---: | ---: |
| 1 | Prometheus | 18929.49 | 20299.48 | +7.24% |
| 1 | key-auth | 24287.50 | 25258.40 | +4.00% |
| 1 | local limit-count | 23339.54 | 24707.44 | +5.86% |
| 2 | Prometheus | 19098.09 | 20693.48 | +8.35% |
| 2 | key-auth | 24901.93 | 25884.72 | +3.95% |
| 2 | local limit-count | 23183.36 | 24985.73 | +7.77% |
| 两轮均值 | Prometheus | 19013.79 | 20496.48 | +7.80% |
| 两轮均值 | key-auth | 24594.72 | 25571.56 | +3.97% |
| 两轮均值 | local limit-count | 23261.45 | 24846.59 | +6.81% |

均值行先对两轮各实现的QPS取算术均值，再计算Native/Lua−1；不是挑选较好的轮次，
也不是将三个不同插件合并成总收益。小数先按原始数值计算，最后显示时四舍五入。
这些是两个配对观察，**不是统计显著性结论**；没有足够独立重复来估计稳定置信区间。
未达目标仅适用于这个冻结候选集、实现和运行条件，不能据此宣称C/Native已达绝对极限。
上一轮极简或全局指标矩阵也不作为当前完整单插件实现的性能上限。

### 样本完整性与身份核对

逐一核对全部12个样本：duration_seconds均为60，error_rate均为0；wrk原文没有socket或
非2xx错误，QPS与JSON一致；12份error.log均为空。父级和容器cgroup的nr_throttled、
throttled_usec均为0。父级CPU配额仍为`400000 100000`，不能将“这批没有节流”推广到任意负载。

[第一轮environment](results/exhaustive-20261031/final-round-1/environment.json) 与
[第二轮environment](results/exhaustive-20261031/final-round-2/environment.json)
除variant标签外相同；两份build.json也完全相同，见
[第一轮build](results/exhaustive-20261031/final-round-1/build.json) 和
[第二轮build](results/exhaustive-20261031/final-round-2/build.json)。
稳定源码revision均为`473043623c161966f3f6ef4e8f12d2d8667a12d2`，16个已记录的
稳定源码hash与本地固定checkout相符；28个Native源码/配置hash既互相一致，也与构建清单相符。
运行时版本、configure参数和依赖清单相同；薄runtime.lua和归档模块的实体SHA-256分别为：

- runtime.lua：`7402e0fbe6294796e82550add0e2e0e978b7773271e526f9387961f80b3b4a41`。
- Native模块：`3e95c0d75a8f292fc9193943cc0b44d360deab867692af4f3db6145b8e798f41`。

两轮均为`-O3`、无LTO、无sanitizer/函数/route插桩。六份Native生成配置均只开启选定六项
及既有native_filters/proxy；六份Lua配置不启用Native指令。每份插件allowlist只有对应单插件，
ai未加载。这里核对的是归档清单中记录的源码、薄runtime和模块身份，不把版本字符串当成
所有运行时DSO的二进制hash，也不改变“共同历史运行时、非原装稳定发行镜像”的比较限制。

### CPU与驻留内存辅助观察

下表分别取两轮worker CPU/请求及每轮worker RSS峰值的算术均值；RSS不是Lua存活堆，
这些短样本也不能证明长期内存稳定或无泄漏。

| 独立路由插件 | Lua CPU µs/请求 | Native CPU µs/请求 | Lua峰值RSS均值 MiB | Native峰值RSS均值 MiB |
| --- | ---: | ---: | ---: | ---: |
| Prometheus | 52.54 | 48.75 | 31.95 | 19.97 |
| key-auth | 40.62 | 39.05 | 31.13 | 17.26 |
| local limit-count | 42.92 | 40.20 | 31.65 | 19.18 |

p50/p95/p99原值保留在每个results.json。wrk采用闭环负载，两种实现各自在其实际达到的
吞吐下测延迟；不能把这些分位数解释成固定到达率、相同负载下的SLO改善，也未用它们选择最佳结果。

## 组合本身的增量控制

在同一O3二进制、同一Prometheus场景下，再运行“BASE / 选中组合 / BASE”三个独立60秒样本。
BASE只开上一轮保留的lazy_rate_json。结果分别为 **20550.02 / 20913.26 / 20649.30 QPS**；
组合相对两侧控制均值为 **+1.52%**。因此本轮局部优化的增量是小幅观察值，不能把相对Lua的
全部7.80%算作本轮新收益，更不能将单项筛选百分比相加。

原始记录：[前控制](results/exhaustive-20261031/combo-control-0/results.json)、
[组合](results/exhaustive-20261031/combo-selected/results.json)、
[后控制](results/exhaustive-20261031/combo-control-1/results.json)。这仍是单个组合观察，
并非显著性检验；此项只测Prometheus，不把增量外推给另两个插件。

## 分配、JIT与剩余成本

以下为15秒诊断负载中的10秒窗口，**不是吞吐验收样本**。
[diagnostic-summary.json](results/exhaustive-20261031/diagnostic-summary.json)保存归一化结果。
所有47个正式吞吐样本均为60秒、无错误；其中仅首批header dirty-bit筛选发生1次父级节流
（34227微秒），其余46个为0。最终12个稳定版配对均无节流。

### Lua分配器

使用公开lua_getallocf/lua_setallocf包装原allocator，原callback和userdata完整转发；正常stop
与VM关闭guard均恢复原pair。[生命周期检查](results/exhaustive-20261031/allocator-lifecycle.log)
通过重复start拒绝、正常stop/restart、显式guard恢复及遗漏stop关闭；关闭成功不等于未插桩
LuaJIT内部无泄漏。三个测量窗口均无分配失败、计数未溢出且allocator_restored=true。

| Prometheus诊断 | 新分配调用/接收请求 | 全部allocator调用/接收请求 | 新分配申请字节/接收请求 |
| --- | ---: | ---: | ---: |
| alloc-base | 130.86 | 261.64 | 8121.85 |
| alloc-selected | 124.90 | 249.79 | 6940.47 |
| alloc-lua | 126.12 | 251.93 | 7033.61 |

相对同O3 Native BASE，选中组合新分配调用减少约4.55%，新申请字节减少约14.55%；
它与稳定Lua的分配量已接近。原始证据：[BASE](results/exhaustive-20261031/alloc-base.raw)、
[选中Native](results/exhaustive-20261031/alloc-selected.raw)、[Lua](results/exhaustive-20261031/alloc-lua.raw)。
这些是VM allocator请求，不是逻辑对象数、物理malloc字节、RSS或NGINX请求池分配；
不包含JIT机器码映射和外部FFI malloc。分母取公开ngx_stat_requests差值，是接收请求数，
含同master后台HTTP及窗口边界的在途差异；不把这些总数直接换算成CPU成本或分配站点排名。

### JIT与trace

选中Prometheus的[jit.p输出](results/exhaustive-20261031/jit-selected-metrics.raw)约为
C状态82%、Compiled 9%、GC 6%、Interpreted 4%（四舍五入）。认证/限流C状态分别约94%/92%。
**C状态含NGINX、系统调用附近执行、Lua C API和运行时，绝非“本C模块占了82% CPU”。**
Prometheus中GC样本常落在observe/lookup_or_create一带；没有分配调用栈，不能据此认定某个
第三方函数独占全部分配成本。第三方已有优化的核查见前文。

[trace诊断](results/exhaustive-20261031/traces-selected-metrics.raw)记录1038次exit、3次abort，
abort位于exporter.lua:878/754的“NYI: return to lower frame”。堆快照约1324–2684 KiB，
不是累计分配字节。hooks会改变执行，exit次数也不等于C/Lua往返次数或其CPU代价；未编译路径
本来就可能不产生trace exit，不能凭这些次数宣称桥接成本已经消失。

### registry、阶段与CPU取样

[函数结构计数](results/exhaustive-20261031/functions-selected-metrics.functions.json.raw)
以142358次ac_http_prepare归一化：ac_push约69次/请求，上轮相同Prometheus形状约77次；
ac_var_assign逐次路径约7次，另11次仍由batch_assign完成，变量写入未删除。
ac_request约10次、ac_read_var约8次、ac_route_context约3次仍保留；池fetch/return各约1次。
这能证明重复查找减少，不能证明每次调用成本相同。函数插桩的self/inclusive包含插桩税和
被调用的外部C子树，不作为整个worker的CPU占比。

[PC诊断](results/exhaustive-20261031/pc-selected-metrics.log)有2498个process-CPU样本，
约36.6%落在writev附近、3.2%在recv附近，同时可见字符串、分配、GC、hash/变量查找。
这是最近ELF符号、没有调用栈；系统调用期间的信号投递和被剥离符号会造成偏差，
尤其不能把libc的最近符号标签当作已证实的NSS热点。该结果支持继续分离内核/网络与用户态
成本，不能给各函数精确分摊端到端耗时。

## 当前结论与下一步

已完成本轮冻结的16个运行时方向、3个编译变体；全部正负结果、测试失败及修复均保留。
较大部分可重复查找和分配已经减少，但同构建组合控制只有+1.52%的单次增量；
对稳定Lua的两轮均值为+7.80%/+3.97%/+6.81%，没有达到30%–40%。
这能排除“上述局部手段已经足以达标”的说法，**不能证明当前实现已到绝对性能极限**。
也没有遍历所有开关组合、硬件、并发、路由规模或运行时版本。

优先下一步是固定专用CPU/共同发布依赖，增加交错重复并取得包含内核与Lua分配站点的完整
调用栈，确认真正还可移除的请求成本。当前简单路由算法远低于5%门槛，不优先换树；
大路由表须重新测。若继续追求30%–40%，需要证据支持相对Lua总请求成本减少约23%–29%，
并评估更粗粒度阶段/插件框架接口；这不能通过忽略getter可变性、yield、配额共享、指标、
重试、keepalive或对象生命周期来换取。更大的执行模型变化需要新的实现和差分验证，不能
从本轮单项计数直接承诺收益。

共同固定依赖并非原装发行镜像；URI转发策略、结束后alias与可替换runtime wrapper边界均已
明确记录。业务插件源码未改，所有实验开关默认关闭，未选方向可按原始配置独立复查。
诊断结束后已恢复与两轮配对完全相同SHA-256的普通O3模块，未留下插桩构建。

## 交付与完整性

- [证据哈希清单](results/exhaustive-20261031/MANIFEST.json)列出本轮归档的文件、字节数和SHA-256，清单不包含自身。
- [最终完整性检查](results/exhaustive-20261031/final-integrity.json)核对了47个60秒样本的原始wrk请求数/QPS、零错误日志、构建与源码哈希；历史229份和上一轮182份证据均未改变。
- [选中配置](results/exhaustive-20261031/selected-nginx-directives.conf)、[构建元数据](results/exhaustive-20261031/selected-build.json)和[最终二进制](results/exhaustive-20261031/selected-ngx_http_apisix_core_module.so)对应最终配对。源码为`candidate-module-source/`及`candidate-runtime.lua`；首批13项另有`screen-*`快照。
- `harness/`保存交付时的工具与测试，`runner-logs/`保留控制进程的失败、恢复和完成记录。历史首批测试使用过程中有已记录的修复，不能把最终工具快照当作所有早期运行的逐字脚本。
- 修改留在`apisix-next-v4-llm`工作区，尚未提交或推送；原`apisix/`业务代码未修改。
