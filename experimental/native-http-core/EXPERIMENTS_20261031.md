# 2026-10-31：单插件场景与底层成本实验

本轮从研究分支提交 `cd0fb07a1c5970ebcb6e0e3c073639c8e8c256a0` 开始。
历史 `RESULTS.md` 和 `results/` 保留原样；其中全局 Prometheus 的四场景数据
不能替代本轮路由上单独启用 Prometheus、local limit-count、key-auth 的比较。
用户指定的主基线是测试时最新稳定发布版的原版 Lua 核心，须记录官方 tag/commit。
同一新二进制关闭实验开关的 native 路径另作优化消融控制，二者不可混称“基线”。
关闭开关保留旧算法路径，但仍包含本轮共享 helper/入口重构，不声称是旧提交原二进制。
30%–40% 是研究目标，不是已测收益或验收预设。

## 修改前的热点排序与证据边界

下表按可消除成本、适用场景和语义风险排序，不是已经测得的 CPU 排名。
代码位置以函数名为稳定定位，行号会随实验变化。

| 优先级 | 最终路径证据 | 已有优化 / 尚存成本 | 本轮处理 |
| --- | --- | --- | --- |
| 1 | `module/request.c:ac_native_var_fetch`、`ac_prefetch_log_vars` | typed runtime C API 和日志批量预取已存在；普通字段仍逐次规范化、hash、NGINX 查找，固定预取名单仍按字符串分派 | 初始化期预解析固定变量索引，保留动态 fallback 和 getter/cache 优先级 |
| 2 | `module/utils.c:ac_rate_fixed` | 已是 C JSON 编码，Lua 字符串延迟创建；没有 observer 时仍分配输出 buffer、转义和完整编码 | 对安全整数形状保存请求内快照，NGINX/Lua observer 真正读取时物化 |
| 3 | `runtime.lua:M.balance`、`module/upstream.c:ac_balance_enter/ac_balance_finish` | 直接 balancer C API 已存在；正常首个 attempt 仍经过两个 Lua→C 调用，两次 `ac_request` | 只合并正常首个 attempt 的空传输帧；保留 retry 的 Lua before_proxy 回调 |
| 4 | `module/module.c:ac_request/ac_cleanup`、`module/request.c:ac_set_vars/ac_release_vars` | 外层 `api_ctx` 每请求新建和 registry ref/unref；`ctx.var` 和其 cache 已用第三方 tablepool 复用 | 先核算存活范围，不在本轮盲目复用请求对象 |
| 5 | `module/plugins.c:ac_next/ac_complete/ac_leave` | start/step 已合并阶段转换；回调前后仍需维护 `_plugin_name`、配置上下文和 registry 读取 | 不重做阶段合并，不跳过 log 清理或可观察阶段 |

归档的 `results/profile-functions-before-metadata-cache.log` 有 78,228 次
`ac_http_prepare`、78,228 次 `ac_balance_finish`、8,057,116 次 `ac_push`
（约 103 次/请求）和 1,329,764 次 `ac_request`（约 17 次/请求）。
这是元数据缓存、C filters、日志预取之前的 instrumented 数据。
其中 `ac_rate_info` 的 9.2%、`ac_balance_finish` 的 1.7% 是受 instrumentation
影响的 C 子树 self 比例，不是当前 worker CPU 占比，也不是可直接兑现的收益。

`results/before-native-access/profile-native-combined.txt` 的 compiled 11%、
C 74%、interpreted 8%、GC 7%，与 Lua 的 compiled 29%、GC 10% 均属旧轮。
它们不是最终路径 trace exit 计数；不能据此声称当前 GC 是首要瓶颈。
本轮应补最终路径状态采样与 trace 诊断，吞吐测试必须恢复无剖析构建。

## 可静态核对的边界与分配预算

在没有拒绝、重试、脚本、额外 filter 的普通代理请求中：

- access 的薄 Lua 帧调用一次 `engine.start_phase`；每个实际执行的 Lua 插件
  回调增加一次 `engine.step`。C 内部完成 rewrite/access/before_proxy 的调度。
- 原正常 balancer 有 `balance_enter`、`balance_finish` 两次 Lua→C 调用。
  fused 实验变成一次，少一次 `ac_request` 查询；NGINX/ngx_lua balancer 入口、
  peer/timeout/keepalive 调用仍完整执行。不能将其称为删除 balancer 阶段。
- log 仍执行一次 `start_phase`，每个 log 回调一次 `step`，然后释放变量、tracing
  状态及 ngx_ctx 链接。无 log 插件也不能直接删除此入口，因为它承担清理和上游反馈。
- direct C filters 已消除本场景的 Lua header/body 入口，本轮没有重复计算其收益。
- `ac_request` 创建请求池中的 C 上下文、cleanup 记录和 metadata-match 数组，
  并创建一个 32-hash-slot 的 Lua api_ctx 表及一个 registry ref。
  请求池分配与 Lua GC 堆分配是两类成本，不能将 pool 分配次数当作 malloc 次数。
- `ac_push` 是 registry `rawgeti`；它本身不创建新表。跨阶段对象必须保持根引用，
  不能只因调用次数多就移除 registry ref。

本预算不包含插件内部 SDK 调用，也不把 C 内部函数调用等同于 VM 边界。
带认证/限流的 cache miss、配置发布和重试需另外计数。

## 已核实的第三方优化

直接下载并检查 Dockerfile 固定的
`nginx-lua-prometheus-api7-1.1.0-1.src.rock`：

- `prometheus.lua:381–457` 的 `lookup_or_create` 已按 label 值树缓存完整 metric
  名称；`:396–401` 特意避免 ipairs，以便外层循环生成更长 trace。
  不应再把“增加指标标签缓存”列为未尝试的无成本收益。
- `prometheus_resty_counter.lua:26–33,96–104` 已有 worker 本地增量缓存和 timer sync，
  counter/histogram 不是每次都直接更新共享字典。
- 该版本 `incr` 仍为每次增量创建 `{v=step,t=exptime}` 表；
  `prometheus.lua:594–635` 的 histogram observe 更新 count/sum/Inf 及桶。
  这解释了存在插件/依赖分配成本的可能性，不证明其当前 CPU/GC 占比。
  本轮不修改第三方指标逻辑来混入核心优化收益。
- `module/request.c:ac_set_vars/ac_release_vars` 已调用第三方 tablepool 和 table.clear，
  与 `apisix/core/ctx.lua:447–465` 对应。`ctx.var` 与 `_cache` 复用不是新方向。
- `apisix/init.lua:889,1632` 的原版外层 api_ctx 也有 tablepool fetch/release；
  native 外层 api_ctx 尚未复用。后续可针对同一生命周期做有界池实验。
- 固定的 ngxvar v0.5.2 `http.lua` 已对 uri/host/地址/scheme/timing 使用 typed C API，
  `request_length`/`bytes_sent` 仍是 ngx.var 后 tonumber。其 wrapper 与 v0.5.3
  相同（SHA-256 `28ed9377412d7fc256dc958b66de7300014db38165aa0e68e77c82b81fa0fcca`）。
  NGINX 1.27.1 `ngx_http_variables.c:689–692` 对已有 index 的名称查找本来就进入
  `ngx_http_get_flushed_variable`；本实验节省的是名称解析，不是发明变量缓存。
  不能改用裸 `get_indexed_variable` 而保留已过期的 no_cacheable 值。

可复核的下载哈希：

| 文件 | SHA-256 |
| --- | --- |
| rock | `f9b867c3926c6bdd4d5303b54ce8266b0e77dafa6bd1bd3833c14d77b6b68ceb` |
| prometheus.lua | `431906affc135bcdf632c80da936088d15486ff0b0b207a0c29c1deb67e8d01d` |
| prometheus_resty_counter.lua | `749ecb10c6dd2060dbf081c93005a87f757679ff2eb81c99989a2553bd96bb01` |

## 实验开关、风险和回退条件

实验均默认关闭。HTTP main 配置中逐项使用 `on` 或 `off`，并归档生成配置。
开关关闭时保留旧路径，不删除负收益数据。

### 变量预解析

成本来源是固定字段重复分派/规范化/hash；不更改插件 getter 的执行时机。
动态名称、numeric captures、自定义 getter、false cache 和头修改后的失效仍走原语义。
`apisix_core_log_var_plan on;` 启用，`off` 恢复通用名称查询。
`ac_request_variables` 在 postconfig 中预解析五个字段的 hash，并仅给
NOCACHEABLE 字段新增 index；status 与 upstream_response_time 继续用 typed C API。
在七个字段均无 cache/custom getter 的一次 log 预取中，少七次通用字符串分派链和
五次 lowercase/hash 计算和四次按名查找；有 cache/getter 时仍优先处理原值。
`tests/test_log_var_plan.py` 检查 bytes_sent 早读后晚读刷新、cached false、自定义
getter 和最小配置变量声明；这些是需要执行的正确性检查，不是已通过的结果。

专项测试发现首版给 `bytes_sent` 新增索引后，提前读取会冻结其值：NGINX 对它没有
设置 NOCACHEABLE，`get_flushed_variable` 不会刷新。这是实际失败；原始日志保存在
`results/single-20261031/verify-log-plan-failed-1.log`。修正后可缓存字段保留按名
查询，用预计算 hash 省去重复规范化，也保留配置已自行索引时的缓存语义。

### 限流诊断按需物化

`apisix_core_lazy_rate_json on;` 启用，`off` 恢复 eager C 编码。
快照必须拥有字符串数据，不能引用可变 Lua table 或已经释放的 Lua string。
NGINX indexed/cached variable、ngx.var、ctx.var、同请求更新和显式赋值都是 observer。
任何 observer 得到旧 JSON、少字段、不同转义/精度即回退；generic fallback 必须保留。

### balancer 首次调用融合

`apisix_core_fused_balancer on;` 启用，`off` 保留两次边界调用。
`engine.balance_start` 复用原 enter/finish 的 ctx helper，初次成功时在同一个 C 帧
完成 peer 设置；返回 nil 表示已完成。失败 status/body 仍由原 emit 路径处理。
retry 返回 true，薄 Lua 帧继续执行 before_proxy，随后调用 finish/recreate_request。
旧的 `balance_enter` 和 `balance_finish` API 仍保留。

不跳过首次 access 中的 before_proxy，不改变重试次数、超时、错误回报、
keepalive pool key/CRC/size/idle/requests。已有 `tests/probe.lua` 验证首次
before_proxy 在 access、后续在 balancer，以及 peer 地址/变量；必须在开关两态运行。
任何 retry/keepalive 差分即回退。若配对单样本不显示可信收益，不宣称优化已成立。

## 请求对象复用为何暂缓

原版已有 tablepool，值得针对 native 的外层 api_ctx 做后续实验；但直接保存
`ac_request_t`、NGINX request/pool 指针到 worker 缓存会跨越所有权边界。
必须等所有 request cleanup、in-flight generation pin、yield continuation、插件可见
别名和异常路径均完成后再复用。提前在 log 清空 api_ctx 可能使后续 cleanup 或仍持有
别名的回调读到另一个请求的状态。正确的实验应复用 Lua 表容量并清理引用，
不复用请求池指针，不建立无界 registry root 池。

## 本轮实测与验证

官方 release API 在本轮返回 3.19.0（2026-09-28 发布），固定 commit
`473043623c161966f3f6ef4e8f12d2d8667a12d2`。两个实现均加载该 tag 的原始插件及辅助
源码，`ai` 从显式插件名单排除。原研究分支的 `apisix/` 没有修改。
共同运行时仍为历史固定镜像 OpenResty 1.27.1.2 / APISIX runtime 1.3.2；其中
Prometheus 库 1.1.0 不同于稳定版 rockspec 声明的 1.0.0。因此这是稳定版源码在
共同固定依赖下的核心对照，**不是未经修改的官方发行镜像对照**。

### 当前路径剖析（全部实验开关关闭）

短诊断每次 15 秒，采样窗口约 10 秒，不作为压力验收，不抓取指标；吞吐测试另用
无剖析构建，每个样本预热 5 秒、正式 60 秒。原始日志/配置/哈希位于
`results/single-20261031/profile-*`。

| 单插件 | 实现 | C 状态 | compiled | interpreted | GC |
| --- | --- | ---: | ---: | ---: | ---: |
| Prometheus | Lua | 61% | 22% | 9% | 7% |
| Prometheus | Native off | 81% | 8% | 4% | 7% |
| limit-count | Lua | 70% | 18% | 8% | 3% |
| limit-count | Native off | 89% | 4% | 3% | 4% |

状态采样经四舍五入。C 占比增加包含实际迁到 C 的工作，不能全部算成语言边界浪费。
Prometheus 的 Lua GC 样本主要关联 `observe`；两实现 GC 均约 7%，没有证据说本轮
Native 已解决指标分配问题。trace hooks 中 Native Prometheus 有 7 次 abort，均为
`NYI: return to lower frame`，1,095 次 texit；限流 0 次 abort、179 次 texit。
它们不统计全部 C/Lua 往返，不能换算 CPU 时间；没有据此观察到持续的大量 trace abort。
Lua heap 快照范围分别 1,454–2,680 KiB 与 1,431–2,598 KiB，只是存活堆量，不是分配速率。

最终选定的 lazy JSON 限流路径另做同样短诊断：C91%、compiled3%、interpreted3%、
GC3%；0次abort、185次texit，heap约1,563–2,667 KiB。原始证据
`profile-selected-limit-*` 与关闭开关的剖析分别保存。单次采样的小幅比例变化不能
证明GC减少的精确贡献；剩余较高C占比仍需调用栈/边界成本工具分解。

| Instrumented 调用预算 | Prometheus（138,449 请求） | limit-count（148,507 请求） |
| --- | ---: | ---: |
| `ac_push` registry rawget / 请求 | ≈77 | ≈71 |
| `ac_request` 上下文查询 / 请求 | ≈10 | ≈10 |
| `ac_var_assign` / 请求 | 18 | 18 |
| `ac_read_var` / 请求 | 8 | 8 |
| `ac_native_var_fetch` / 请求 | ≈8 | 1 |
| `ac_route_context` / 请求 | ≈3 | ≈3 |

每个成功请求仍有 2 次 phase_start、1 次 step、1 次 balance_start 和1次
balance_finish 共 5 个显式 engine 入口；fused 实验少一个。还有插件内部 SDK 边界。
函数 profiler 用墙钟且只扣已 instrument 的子调用，因此 self 包含未 instrument
的 Lua/库/下游 NGINX 工作及调度等待。比如 `ac_native_body` 虽居 self 首位，主要
调用 `next_body`，不能拿其 18.9%–25.4% 去指认 wrapper 本身的优化空间。

限流 `ac_rate_info` 的 instrumented self 约7.2%，`ac_rate_fixed` 只有约0.14%。
原因是稳定版插件没有 detail 参数，关闭开关时 native 走通用 table/cjson 路径。
稳定版 Lua 核心自身的简单 JSON 已用 `string.format`。所以本轮限流实验覆盖
“简单形状不再建中间表和编码”以及“按需编码”，不能把收益仅归因于旧 fixed encoder 的延迟。
分支的丰富 JSON 格式仍单独做差分与 sanitizer 回归。

### 正确性

`verify-off.log`、`verify-on.log` 各记录六配置 1,967 次 HTTP 检查及真实插件差分；
`verify-sanitizers.log` 是修正 bytes_sent 后全部开关开启的相同 ASan/UBSan 回归。
模块外的运行时 DSO 未 instrument，leak detection 关闭，不是长期无泄漏证明。
`verify-log-plan-failed-1.log` 保留首次失败；fixed/indexed 日志与后续
`verify-log-plan.log` 验证未索引和已有索引两种缓存语义。
`verify-single-off.log`、`verify-single-on.log` 在稳定版插件下检查路由级指标精确增量、
认证/隐藏凭证、限流/重置、双 worker 共享 5/27 配额以及真实连接复用。
完整 APISIX Test::Nginx 未运行，仍不声称支持范围外能力。

### 60 秒配对结果

以下消融均是相同新二进制、相同稳定版插件的 Native off/on 对照，各一条60秒样本：

| 实验及单插件场景 | off QPS | on QPS | 变化 | off/on CPU μs/请求 | 决定 |
| --- | ---: | ---: | ---: | --- | --- |
| 日志变量预解析 / Prometheus | 20,467.10 | 20,323.72 | -0.7% | 48.79 / 49.13 | 最终关闭 |
| 首次 balancer 合并 / key-auth | 23,956.13 | 23,831.02 | -0.5% | 41.68 / 41.89 | 最终关闭 |
| 按需诊断 JSON / limit-count | 22,220.02 | 23,158.38 | +4.2% | 44.96 / 43.13 | 纳入最终候选 |

变量和 JSON 先off后on；balancer 先on后off。正负结果都保留在各自 `*-off/`、
`*-on/` 目录。没有把0.5%–0.7%差异认定为稳定退化，也没有把+4.2%认定为统计显著。
本负载不会读取诊断 JSON；按需编码在每次都读取 JSON 的生产配置中是否仍受益未知。
三个实验开关在模块中继续默认关闭。最终 Native 配置只显式启用
`apisix_core_lazy_rate_json on;`，另外两个保持off。

最终 Lua/Native 三场景配对的原始结果在
[final/results.json](results/single-20261031/final/results.json)。**没有达到30%–40%目标**。

| 路由单插件 | 稳定版 Lua QPS | Native QPS | 吞吐变化 | Lua / Native CPU μs/请求 |
| --- | ---: | ---: | ---: | --- |
| Prometheus | 19,374.67 | 20,600.25 | +6.3% | 51.57 / 48.46 |
| key-auth | 23,801.04 | 23,844.21 | +0.2% | 41.91 / 41.91 |
| local limit-count | 23,834.17 | 23,049.47 | -3.3% | 41.90 / 43.33 |

| 路由单插件 | Lua / Native p99 ms | Lua / Native 结束 RSS MiB |
| --- | --- | --- |
| Prometheus | 3.312 / 2.621 | 31.28 / 19.86 |
| key-auth | 2.202 / 2.074 | 31.40 / 17.51 |
| local limit-count | 2.176 / 2.268 | 32.51 / 19.07 |

单网关 worker 绑CPU0，上游绑CPU1，wrk两线程32连接绑CPU2–4，HTTP/1.1 keepalive，
后端3字节 `ok\n`，有效key，限流配额1,000,000,000/600秒。
Prometheus 仅在其独立场景每秒抓取；其他两场景不加载该插件，也没有全局规则。
主机 AMD EPYC 9V74、可用5个CPU编号但父级配额4核；历史机器没有此配额，不能跨轮比较绝对QPS。
六条最终样本和六条消融样本均无请求错误、worker替换、新增error/alert或宿主配额节流。
这些检查不能排除共享主机竞争。每场景单次配对，没有统计显著性/长期稳定性结论。
p99是各自吞吐下的闭环延迟，CPU只计网关worker，RSS不是泄漏率。

最终执行期间外层工具会话标识丢失，宿主 `final.log` 仅保留启动部分；容器内部的
benchmark进程持续运行并完成全部六条样本。已检查落盘结果、原始wrk、每样本错误日志
与配置并直接复制归档，没有补跑、拼接或把不完整样本计入结果。
实测模块 `module.so`、当前C源码和runtime哈希与 `final/environment.json` 一致。

按需JSON改善了Native自身，但该场景仍落后于稳定版Lua；认证几乎持平。
这轮实验不能支持“换成C即可提升30%–40%”。两个未获收益的实验继续默认关闭，
按需JSON也保持显式启用，便于按真实observer负载决定是否使用。

### 仍不确定与下一步

1. 优先研究 `http.c:ac_var_assign` 每请求18次赋值：每次重复 registry → api_ctx → var
   → cache 查找，可在已有不 yield 的 C 操作内部批量取句柄、写字段。保留 forwarded
   header 的原始/归一化视图、无效化和 getter 语义。这与已经实现的日志预取不是同一工作。
2. `ac_read_var` 每请求8次读取也可试预解析描述符，必须避免本轮 `bytes_sent` 的索引缓存陷阱。
   函数计数支持调查方向，不给出可获得的 CPU 百分比。
3. `routes.c:ac_match_start` 即使没有 Lua route filter 也创建 opts/matched 两张表；
   可考虑只在真正调用 filter 时创建 opts，仍保留插件可见 matched 身份。
   外层 api_ctx 有界复用需先证明别名、cleanup、yield 和配置代际生命周期。
4. 当前 profiler 没有精确 Lua 分配速率、完整 native 调用栈或 VM 边界周期计数。
   下一步可做受控分配统计/调用栈采样；instrumentation 数据与验收构建必须分开。
5. 30%–40% 吞吐提升意味着在相同瓶颈下减少约23%–29%的每请求耗时。
   当前证据不能证明还存在这么多可删除的核心成本；也不能把早期省略职责的原型当作上限。
   若要判断小收益是否稳定，应事先约定交错重复配对，仍保持每个压力样本60秒。
