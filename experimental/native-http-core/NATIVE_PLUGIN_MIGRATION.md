# 从 C 核心到 C 插件：直连与渐进迁移设计

本文记录已实现的 v2/v3 协作方式，并提出后续演进契约。性能和验证事实引用
[研究入口](RESEARCH_INDEX.md)、[v2 报告](NATIVE_KEY_AUTH_V2.md)和
[v3 报告](HYBRID_PROMETHEUS_20261031.md)；标为“后续”的能力尚未实现。

## 为什么保留混用

迁移核心之后，插件可以逐个演进：成本高、语义边界清楚的插件迁到 C，已有 Lua 插件继续
提供功能。已完成的例子是 C key-auth 与未修改的 Lua Prometheus 在同一 worker 中协作。
这使核心和插件的迁移节奏可以分开，也允许分别观察业务移植与接口适配的成本。

v2 对照表明，只改认证业务还会保留 Lua 阶段帧、上下文和接口转换；让同一份 C 认证直接
使用 C 请求状态，又获得了明显增益。v3 表明，保留一个需要 Lua 的插件时仍可省去其他阶段
的 Lua 工作。但混用有实际成本，且目前只实现 log 桥接，尚无通用的任意阶段混合执行器。

Lua VM 按 worker 初始化。本文所说的上下文“创建”指请求的 api_ctx、变量和匹配视图，不是
每个请求新建 Lua VM。C 路径也仍有 NGINX request pool、字符串及其他原生分配。

## 已实现的两条路径

### C 核心与 C key-auth 直接协作

```mermaid
flowchart LR
    A[NGINX access handler] --> B[C 请求准备和路由]
    B --> C[C key-auth]
    C --> D[C 上游准备与 peer callbacks]
    D --> E[NGINX proxy 与 keepalive]
    E --> F[C log 反馈与清理]
```

[pipeline.c](module/pipeline.c#L28) 的 `ac_pipeline_access` 依次调用
`ac_http_prepare_native`、`ac_match_native`、`ac_key_auth_run`、`ac_upstream_prepare_native`。
认证直接读取按配置代际编译的 C 密钥索引，并把消费者保存在原生请求状态中。
拒绝请求显式结束，不允许继续执行 proxy；HEAD 和请求体清理也在验证范围内。

路由仍使用 [routes.c](module/routes.c#L548) 的 C 压缩前缀树和原有优先级规则，
没有在请求中调用 lua-resty-radixtree，也没有为本专项更换算法。
本轮没有干净的路由 CPU 占比证据，不能声称它已超过或低于 5% 的更换阈值。

[upstream.c](module/upstream.c#L526) 直接对接 peer 选择/释放和 NGINX stock keepalive，
复用已有原语，没有另写连接池。重试、超时和反馈在已限定的静态 HTTP 配置下验证。
生成配置移除了业务 Lua access/filter/log/balancer hooks，所以这条请求热路径不执行 Lua。
配置加载、schema/defaults、初始化和 watcher timer 仍使用 Lua。

### C 请求路径＋原版 Lua Prometheus

```mermaid
flowchart LR
    A[C 请求准备与路由] --> B[C key-auth]
    B --> C[C peer callbacks 与 NGINX proxy]
    C --> D[NGINX log 阶段]
    D --> E[按需创建 Lua 上下文视图]
    E --> F[既有调度器执行 Lua Prometheus]
    F --> G[释放变量和请求视图]
```

[runtime.lua](runtime.lua#L103) 的 `native_log()` 先调用
[ac_pipeline_log_start](module/pipeline.c#L165)，再进入既有 `run("log")`。
桥接根据 C 已确定的状态发布匹配路由、消费者、节点、响应来源及认证前 URI 视图；
请求转发状态仍由 C 持有。原版 `prometheus.lua` 的请求回调只有 `log = exporter.http_log`，
因此当前请求只需在 log 阶段进入 Lua，无需恢复 Lua access 或 balancer。

Prometheus 的全局/路由覆盖、metadata filter、extra labels、SDK getter/cache 继续由既有
接口和调度器处理。它运行在同一 worker 的同一请求中，没有增加 HTTP 转发层。
上游反馈有一次性标记，Lua log 也有一次性入口，避免重复反馈或重复记录指标。

log 视图使用 [ac_attach_lua_context](module/module.c#L371) 接入既有 SDK，
消费者通过 [ac_key_auth_publish](module/key_auth.c#L289) 发布。
本轮的确切模式是“C key-auth＋Lua Prometheus”，不是通用 C 插件链加任意 Lua 插件链。

## 当前的状态、生命周期与复用

| 对象/职责 | 当前所有权与处理 |
| --- | --- |
| 路由、认证配置、密钥索引 | 配置代际拥有；请求固定所用代际，更新后旧请求仍读旧状态 |
| 转发、消费者、peer 等请求状态 | NGINX request pool 中的 `ac_request_t` |
| Lua api_ctx/var/匹配视图 | 旧适配器提前创建；完整 C 不创建；混合模式到 log 才创建 |
| getter/cache | 沿用 SDK 读取语义；桥接发布已有值，保留自定义 getter 与缓存行为 |
| 请求 Lua 表的池化 | 使用既有请求池开关；完成 log 且所有权检查通过才归还 |
| 异常或未完成 log 的清理 | 保留 GC 路径，不把仍可能有别名的表归还池 |
| 旧配置释放 | 在途请求释放代际引用后，由既有 watcher/清理机制回收 |

池化的目标是减少反复分配，同时保持请求隔离。不能在有挂起协程、请求外别名或在途旧代际
时复用状态；也不能把原生请求 pool 生命周期直接当作 Lua 对象生命周期。
当前兼容承诺针对只读、版本化配置，插件直接修改配置表不属于该模式承诺。

v3 诊断中，旧适配器到混合路径的 `ac_push` 从约 94 降到 41 次/请求，`ac_request` 从 13
降到约 5；Lua balancer 入口为零。但 VM 新分配仍约 124 次/请求，与旧适配器接近。
这说明指标相关工作仍然存在。函数计数不是 CPU 占比；allocator 计数不是物理 malloc 或
整个进程的内存统计。后续要采集 JIT trace exit 和 GC CPU 证据，不能把它们当作已消除。

## 现在可以配置什么

下表列出必要开关，并非可直接运行的完整 nginx.conf；完整配置由测试工具生成。

| 模式 | 必要开关/钩子 | 当前插件范围 |
| --- | --- | --- |
| 既有 C 核心＋Lua 插件 | 使用原阶段适配，native pipeline 关闭 | 原有实验已覆盖的 Lua 插件/阶段 |
| C key-auth＋旧阶段适配 | `apisix_core_native_key_auth on;`，保留 Lua 阶段 hooks | 本专项限定的认证能力，可配原 log-only Prometheus |
| C 请求闭环 | `apisix_core_native_pipeline on;`，移除业务 Lua hooks，配置 C balancer/filters | key-auth |
| C 请求闭环＋Lua 指标 | 上一行＋`apisix_core_lua_log_bridge on;` 和 `runtime.native_log()` 钩子 | key-auth＋log-only Prometheus |

这些新开关均默认关闭；native pipeline 隐含启用 C key-auth。log bridge 必须配合 native
pipeline。启用 Prometheus 却漏掉桥接、引入其他 Lua 阶段插件或其他不支持配置时，编译检查
会拒绝，不会静默跳过业务。[ac_pipeline_compile](module/pipeline.c#L100) 仍是封闭能力检查。

当前完整 C 路径针对 route-only literal key-auth、静态 HTTP 上游。Lua route predicate/script、
认证 metadata 回调/自定义错误响应、secret provider、消费者/组引入其他插件、多认证，以及
tracing、servlet/encoded-slash/trusted-address 扩展等不属于该路径的兼容范围。
DNS/chash/HTTPS、多 endpoint keepalive 容量和连接年龄语义的进一步推广也没有完成。
URI 点路径策略与稳定 Lua 的已披露差异继续存在，不能把范围收窄误写成全部 APISIX 等价。

迁移中的不支持配置有两种处理：当前编译器明确拒绝；或者未来实现并验证对应桥接/原生能力。
本轮没有实现自动切换执行器，也不能假定某个请求可以随意从 C 路径跳回完整 Lua 路径。

## 后续：让插件迁移接口可复用

现在 key-auth 通过专用 C 函数和字段直连。它是一个已验证的样例，尚不是稳定的公共 C 插件
ABI，也没有注册任意外部 C 插件的通用机制。下一阶段可以据此整理下列契约：

| 契约 | 需要先明确的行为 |
| --- | --- |
| 配置编译与销毁 | schema/defaults、资源合并、代际发布/拒绝、原生和 Lua 引用的所有权 |
| 阶段注册与排序 | plugin priority、global/route 覆盖、metadata、消费者合并和执行一次性 |
| 执行返回值 | 继续、拒绝、响应已发送、错误；合法阶段中的挂起/恢复不能靠省略 yield 实现 |
| 请求读写 | URI/headers/consumer/peer 的权威状态，写入后的变量和 SDK 缓存失效 |
| 跨语言视图 | 按阶段/需要发布，保留 nil/false、多值头、自定义 getter 和对象身份约定 |
| 清理与池化 | 正常/异常/断连路径、别名与协程生命周期、跨代际请求隔离 |
| 统计与上游反馈 | 计数、响应来源、重试/多次 upstream、keepalive 和日志恰好执行所需次数 |

配置编译期判断整条执行计划需要哪些 Lua 阶段，再生成所需桥接，是后续方向。它可能减少
无用视图和阶段入口，但当前没有对应的新实现或性能数字。不能只根据插件名字判断它无
副作用；要同时检查配置、metadata、消费者/组和它实际依赖的 SDK。

## 后续：按插件、按阶段推进

1. 先把 key-auth 专用直连整理成可复用契约，保持当前直接调用路径作为对照，防止通用化又
   逐请求引入动态查表、临时 Lua 表和重复状态转换。
2. 对 log-only 混用先测固定 RPS 延迟曲线和 Prometheus 分配/JIT/GC，再判断应优化桥接、
   指标库或迁移指标业务。当前不把“把 Prometheus 全部改成 C”当作已验证方案。
3. 若继续迁移 local limit-count，必须保持跨 worker 共享配额、原子操作、窗口/TTL、拒绝码、
   响应头、诊断 JSON 按需行为和 getter/cache 语义。不能用每 worker 私有计数替代共享配额；
   Redis/滑动窗口等能力也不能未经实现就标为支持。
4. 若引入 Lua rewrite/access/filter 插件，先实现对应阶段桥接，明确认证前后状态、优先级、
   短路、yield/resume 和 filter 顺序，再做同功能测量。当前 log 桥接不能直接承担这些职责。

收益明显的插件可以优先迁移，业务变化频繁或成本低的插件可继续使用 Lua。
这个选择是按部署能力和测量结果做的，不要求全项目一次性改成 C。

## 每次迁移的验收与回退

先列出完整语义范围与明确拒绝项，以原 Lua 插件作同输入差分。验证成功、拒绝、异常、断连、
并发、配置更新/旧代际和所需多 worker 行为；涉及缓存/getter、yield、共享配额、指标、重试、
keepalive 时分别验证对应语义。模块 sanitizer 通过不能替代这些业务断言。

性能至少保留“原 Lua”“C 业务＋旧适配器”“C 业务直连”，混用另加相同 Lua 插件配置，
以便区分迁移层次。每个吞吐压力样本仍为 60 秒，固定资源和顺逆序，保留全部正负数据及
mean/P95/P99/P99.9、CPU/请求、RSS、错误和节流。另测固定 RPS 的延迟，避免把不同到达率
混成延迟收益；多轮、更多路由/消费者、不同并发和硬件用于评估稳定性。

回退必须同时恢复执行器开关和 nginx.conf 阶段 hooks。完整 C 配置已经移除业务 Lua hooks，
不能只关闭 `native_pipeline` 就期望自动恢复 Lua 请求流程。应从工具重新生成目标模式配置，
验证后 reload，或切回固定检查点/二进制。已有配置热更新失败保留旧代际，并不等同于执行器
自动回退。原 Lua 插件源码、旧适配路径和阶段分支都继续保留。

当前结果证明了这条渐进路径在 key-auth＋Prometheus 场景可行。是否迁移更多插件、是否能
保留全部部署能力以及通用化后还能保留多少收益，仍须按上述过程实测，尚无全插件收益保证。
