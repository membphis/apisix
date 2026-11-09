# 第一步：只改内核、key-auth 仍是 Lua（C 内核 vs Rust 内核）

三步走的第一步是"内核换成原生实现，插件仍是原版 Lua"。这是社区最容易接受的一步，后两步都以它为前提。本文回答：第一步用 C 写内核和用 Rust 写内核，结果有什么不同。

- 依据：维护者确认的预研范围，第一步沿用 V1 设计（C native-lua 的同一机制）
- 证据（`results/` 下）：
  - [`bench-five-20261109T023114Z/`](results/bench-five-20261109T023114Z/)：五条路径同一批次，2 轮；
  - [`bench-extra-20261109T025622Z/`](results/bench-extra-20261109T025622Z/)：第一步落入模糊区间后补测的一对（第 3、4 轮）；
  - [`step1-four-rounds/`](results/step1-four-rounds/)：第一步两条路径 4 轮合并后的汇总，由 `tools/analyze_three.py` 从上面两个批次生成；
  - [`bench-step1c-20261109T014915Z/`](results/bench-step1c-20261109T014915Z/)：更早的一个批次，只测了原版 Lua 和 C 第一步；
  - [`validation-step1-20261109/`](results/validation-step1-20261109/)：一致性、响应头、panic 测试和构建信息。

## 结论

**第一步用 Rust 写内核和用 C 写内核，功能相同，性能相同。**

- **吞吐：** 4 轮均值 Rust 16,831 QPS，C 16,841 QPS，相差 −0.05%。逐轮差值为 +2.8%、+5.3%、−6.0%、−2.2%，正负交替，属于噪声。最初一对的差距（+4.05%）落在 3%–8% 的模糊区间，按规定补测了一对，补测这一对是 −4.07%。
- **单请求 CPU：** 饱和时 Rust 高 0.5%，固定速率下高 1.5%，都在噪声范围内。Rust 还有几处每请求的小堆分配没有优化（见"边界"），不利于 Rust。
- **延迟：** 固定速率下 p50 基本相同（C 130 µs，Rust 134 µs）。p99 由排队决定：同一实现 4 轮之间差 3–9 倍，两者的区间重叠。
- **功能：** 47 个语义场景、响应头和配置代际检查都和原版 Lua 一致。
- **门槛 G1 判定：通过。**

**第一步本身带来的性能收益很小。**

- 相对原版 Lua，第一步的吞吐只提升约 3%–9%，随批次变化；按 4 轮均值，C 和 Rust 之间没有差别。单请求 CPU 降低约 3%–7%，RSS 降低约 44%。
- 原因是 Lua 插件和 Lua SDK 适配层仍在热路径上。全原生的 v2 相对 Lua 是 +120%。
- 所以第一步的价值在于证明"内核可以替换、插件不用改"，而不在性能。性能收益要等第二步把插件迁成原生。
- 对选型的含义：过渡期最担心的"Rust 托管 Lua 插件"已经用同一机制实现，性能和 C 相同。代价是 3,847 行、约 87% unsafe 的适配层，以及 Rust 特有的错误传播规则（这一层的 panic 会中止 worker），而不是可行性或性能。

**更正：** 首批实测的 Rust v2 漏掉了 C v2 的响应头过滤器。补上之后在同一批次重测：Rust v2 与 C v2 的吞吐相差 +0.06%（原先报告 +2.18%）；固定速率下单请求 CPU Rust 低约 2%，但两轮方向不一致。G1 仍然通过。

## 对比对象

| 路径 | 内核 | key-auth | 说明 |
|---|---|---|---|
| 原版 Lua | APISIX 3.19.0 的 Lua 内核 | 原版 Lua | 基准 |
| C 第一步 | C 原型 V1（native-lua，PR #1），开同样的六项开关 | 原版 Lua，不改动 | C 预研里的"方案一" |
| Rust 第一步 | 本次实现，沿用 V1 设计 | 原版 Lua，不改动 | `apisix_rs_lua_plugins on` |
| C v2 | C 请求闭环 | C 原生 | 对照：全原生 |
| Rust v2 | Rust 请求闭环（已补上响应头过滤器，见"更正"一节） | Rust 原生 | 对照：全原生 |

五条路径在同一镜像、同一份 NGINX 源码、同一份 standalone 配置下运行，在同一批次里测量。

## Rust 第一步的实现

### 机制：和 C 完全相同

- **Lua 侧不变。** 同一份 `runtime.lua`，同样的入口：`init_by_lua` 里调用 `engine.init`，`init_worker_by_lua` 里调用 `engine.init_worker`，access 阶段调用 `runtime.run("access")`，balancer 阶段调用 `runtime.balance()`，log 阶段调用 `runtime.run("log")`。原版 APISIX 内核的 Lua 模块被同一个 loader 拦截，加载即报错。
- **引擎协议相同。** `apisix.native.engine` 由 Rust 模块提供，函数和语义与 C 一一对应：`start_phase`、`step`、`render`、`balance_start`、`balance_finish`。内核负责阶段调度（rewrite、consumer 路由合并、access、before_proxy、log）、路由、上游准备和选点；Lua 只负责调用插件回调并把结果原样交回内核。
- **SDK 用 Lua C API 实现。** key-auth 用到的部分：`apisix.core` 的 `request`、`response`、`log`、`schema`、`ctx`，以及 `apisix.schema_def`、`apisix.consumer`、`apisix.utils.auth`。逐个函数对照 C 的 `sdk.c`、`request.c`、`response.c`、`consumer.c`、`schema.c` 移植，包括 `ctx.var` 视图、请求头缓存、api_ctx 表池、转发头处理和 consumer 路由合并缓存。
- **其余部分复用 Rust v2。** 路由树、上游选点、配置合同来自 `apisix-rs-core`；配置代际、NGINX 变量和响应头过滤器来自 v2 的胶水层。插件配置的默认值由 Lua 插件自己的 `check_schema` 填写，和 C 一样。
- **选点和长连接走 ngx_lua 的 balancer API。** 和 C 第一步相同（`set_current_peer`、`enable_keepalive` 等），不使用 v2 的原生 peer 回调。

### 与 C 的差异

都在编译期整代拒绝，或者对 key-auth 不可见：

- 配置合同和 Rust v2 相同，是封闭的：只支持 key-auth，不支持 service、consumer group、全局规则、路由级 Lua 谓词等。C 的 V1 适配层是通用的，能跑 Prometheus、limit-count 等多种插件。
- SDK 只实现 key-auth 用到的子集。子集之外的成员（例如 `core.json`、`ctx.var.post_arg_*`、计时变量）会显式报错，而不是静默返回 nil。
- C 的原生 body filter 在这个场景下不做任何事，Rust 没有安装。
- Lua 侧的资源视图只构建 routes、upstreams、consumers 三类。

### 代码规模与开发过程

- `tools/code_stats.py`：Lua 适配层 3,847 行（`step1.rs` 2,260 行、`sdk.rs` 1,267 行、`lua.rs` 320 行），约 87% 位于 unsafe 区域。
- Lua C API 用手写的 `extern "C-unwind"` 声明，相当于 mlua-sys 的原始绑定，没有新增依赖。
- 错误模型需要专门处理：LuaJIT 用栈展开实现 Lua 错误，所以面向 Lua 的 Rust 函数不能用 `catch_unwind`，Rust 的 panic 防护在这一层不起作用（见下文 panic 测试）。
- 开发过程：
  - 编译错误 3 类，clippy/rustc lint 6 类，测试脚本 bug 1 个；
  - 崩溃和内存类问题 0 个，第一次运行就通过了全部功能检查；
  - 有一处设计假设是错的：最初以为 LuaJIT 会把逃出的 Rust panic 转成 Lua 错误，worker 不受影响。panic 测试表明 Rust 会直接中止进程，相关注释和文档已经更正。

## 功能一致性（同一批次）

`tests/test_rust_parity.py` 复用未修改的 v2 测试套件，五条路径依次运行，全部通过：

| 路径 | 47 个语义场景 | 128 并发 / keepalive / 拒绝不进上游 / 重试关闭 | 响应头（`Server`、`X-APISIX-Upstream-Status`） | 配置代际检查 |
|---|---|---|---|---|
| 原版 Lua | 通过（基准） | 通过 | 基准 | — |
| C 第一步（native-Lua，套件自带的对照） | 通过 | 通过 | 与 Lua 一致 | — |
| Rust 第一步 | 通过，与 Lua 一致 | 通过 | 与 Lua 一致 | 通过 |
| C v2 | 通过，与 Lua 一致 | 通过 | 与 Lua 一致 | 通过 |
| Rust v2 | 通过，与 Lua 一致 | 通过 | 与 Lua 一致 | 通过 |

- 47 个场景覆盖 header/query key、大小写、重复 key、匿名 consumer、`hide_credentials`、伪造的 `X-Consumer-*` 头、方法和 host 谓词、重试与超时、转发头清洗、105 个填充头等，比较的是状态码、`WWW-Authenticate` 和上游回显（上游实际收到的请求头和 URI）。
- 响应头比较是本次新增的：成功、401、404、上游 502、上游超时 504，以及内核之外的 `/apisix/nginx_status`。
- 配置代际检查和 v2 相同：请求持有旧代际、新连接用新超时、密钥轮换、不支持的 `filter_func` 让整代被拒并记录 "Lua route predicates"、被拒后恢复。C 第一步是通用实现，支持 `filter_func`，所以不做这一项。
- URI 中的点号段（`/auth/a/../item`）沿用 v2 套件的规则，和 native-Lua 对照比较。
- 第一步的 panic 测试见下一小节。

### panic 测试（测试专用构建，`APISIX_RS_FEATURES=panic-test`）

`tests/test_rust_panic.py`：

| 模式 | 触发 panic 的位置 | 结果 |
|---|---|---|
| Rust v2 | NGINX access 处理器，有 `catch_unwind` 防护 | 连续 20 次 panic，每次返回 500，worker PID 不变 |
| Rust 第一步 | `engine.start_phase`，由 `runtime.lua` 调用的 Lua 侧函数 | 每次 panic 都是连接直接关闭；worker 以 SIGABRT 退出，日志为 `fatal runtime error: Rust panics must be rethrown`；master 重新拉起 worker 后请求恢复正常。共 3 次，worker PID 依次为 13344、13350、13353、13357 |

- 面向 Lua 的函数里不能用 `catch_unwind`：Lua 错误靠栈展开穿过这些函数，`catch_unwind` 遇到外来异常会直接中止进程。
- panic 从这些函数逃出后，LuaJIT 会在协程边界捕获它；但 Rust 规定 panic 不能被外来运行时吞掉，于是中止进程。
- 所以在第一步的 Lua 适配层里，Rust panic 的后果和 C 适配层崩溃相同；C 第一步也没有防护，段错误同样会杀死 worker。要让 Rust 在这一层也有防护，需要像 mlua 那样把每个可能出错的 Lua 调用都包进 `lua_pcall`，保证 Lua 错误不穿过 Rust 代码，再在外层用 `catch_unwind`；代价是更多代码和每次调用的额外开销。
- 测试专用代码放在 `step1.rs` 文件末尾，release 构建的 SHA-256 与实测模块完全相同（`770b3e8b…`）。

## 性能（同一批次，五条路径）

### 方法

- 与 v2 首批测量的方法相同：
  - 吞吐样本：先预热 5 秒，再用 `wrk -t2 -c32` 跑 60 秒；
  - 每个吞吐样本之后，在同一个网关实例上跑 60 秒固定速率样本；
  - 网关 worker 绑 CPU0，上游绑 CPU1，压测端绑 CPU2–3。
- 固定速率沿用首批的取值，便于对照：低档 R_low = 13,300 RPS，五条路径都测；高档 R_high = 29,500 RPS，只测 v2 两条路径，第一步和 Lua 跑不到这个速率。
- 顺序：
  - 五条路径同批次 2 轮：第 1 轮 Lua → C 第一步 → Rust 第一步 → C v2 → Rust v2，第 2 轮倒序；
  - 第一步的差距落入模糊区间后，紧接着补测一对：第 3 轮 C → Rust，第 4 轮 Rust → C。
- 样本质量：
  - 所有样本 CPU 节流 0 次，没有错误，目标速率全部达到；
  - 这一批 VM 偏慢，两档速率都更接近饱和：低档下，第一步路径晚于预定时刻 1 ms 以上发出的请求占 2.8%–9.2%（4 轮），Lua 占 13.8%–13.9%；高档下 v2 占 20%–39%。

### 第一步：C 内核 vs Rust 内核（4 轮均值，括号内为第 1/2/3/4 轮）

| 指标 | C 第一步 | Rust 第一步 | Rust 相对 C |
|---|---:|---:|---:|
| 饱和吞吐 QPS | 16,841 (16,251 / 17,065 / 16,658 / 17,389) | 16,831 (16,704 / 17,962 / 15,655 / 17,004) | **−0.05%** |
| worker CPU/请求，饱和时（µs） | 58.56 (60.63 / 57.67 / 59.16 / 56.78) | 58.87 (59.12 / 54.85 / 63.25 / 58.25) | +0.5% |
| 峰值 RSS（MiB） | 16.6 | 16.5 | −0.2% |
| 闭环 p99，wrk 饱和时（µs） | 3,439 | 3,258 | −5.3% |
| R_low p50（µs） | 130 (139 / 98 / 142 / 142) | 134 (150 / 100 / 157 / 127) | +2.5% |
| R_low p99（µs） | 10,562 (19,952 / 7,226 / 7,864 / 7,204) | 18,348 (41,987 / 19,989 / 4,724 / 6,693) | +74%（噪声） |
| R_low worker CPU/请求（µs） | 62.53 (62.66 / 60.31 / 63.62 / 63.53) | 63.45 (64.34 / 61.10 / 67.00 / 61.35) | +1.5% |

### 五条路径（同一批次 2 轮均值，括号内为第 1 轮 / 第 2 轮）

| 指标 | 原版 Lua | C 第一步 | Rust 第一步 | C v2 | Rust v2 |
|---|---:|---:|---:|---:|---:|
| 饱和吞吐 QPS | 16,181 (16,141 / 16,222) | 16,658 (16,251 / 17,065) | 17,333 (16,704 / 17,962) | 35,552 (34,215 / 36,890) | 35,573 (35,083 / 36,064) |
| 相对 Lua | — | +2.95% | +7.12% | +119.7% | +119.8% |
| worker CPU/请求，饱和时（µs） | 60.96 | 59.15 | 56.98 | 27.77 | 27.75 |
| 峰值 RSS（MiB） | 29.7 | 16.5 | 16.6 | 15.7 | 14.9 |
| 闭环 p99，wrk 饱和时（µs） | 3,602 | 3,578 | 3,286 | 1,827 | 1,827 |
| R_low p50（µs） | 154 | 118 | 125 | 77 (82 / 71) | 75 (77 / 72) |
| R_low p99（µs） | 60,159 | 13,589 | 30,988 | 1,051 (1,002 / 1,100) | 2,218 (3,598 / 837) |
| R_low worker CPU/请求（µs） | 65.01 | 61.48 | 62.72 | 39.66 (41.74 / 37.57) | 38.83 (39.86 / 37.79) |
| R_high p50（µs） | — | — | — | 668 (1,233 / 104) | 138 (87 / 189) |
| R_high p99（µs） | — | — | — | 71,804 (81,182 / 62,425) | 29,693 (35,745 / 23,641) |
| R_high worker CPU/请求（µs） | — | — | — | 31.71 (32.42 / 31.01) | 30.93 (30.60 / 31.26) |

- 这张表里的第一步只有 2 轮，Rust 比 C 高 4.05%；加上补测的一对后，4 轮的结论见上一张表。
- 更早的一个批次只测了 Lua 和 C 第一步（[`bench-step1c-20261109T014915Z`](results/bench-step1c-20261109T014915Z/)，两轮）：C 第一步吞吐 18,279，比 Lua 的 16,932 高 7.96%，CPU/请求低 7.2%。C 预研自己的报告里，同一路径相对 Lua 是 +3.97% 和 +8.81%。所以第一步相对 Lua 的收益在 3%–9% 之间浮动。
- v2（更正后）：Rust 相对 C 的吞吐是 +0.06%（逐轮 +2.5%、−2.2%），饱和时 CPU/请求 −0.1%。固定速率下 CPU/请求，低档 −2.1%（逐轮 −4.5%、+0.6%），高档 −2.5%（逐轮 −5.6%、+0.8%）。p99 两档方向相反：低档 Rust 偏高，高档 Rust 偏低；Rust 低档两轮之间就差 4 倍。属于噪声。

### 门槛 G1 的判定

- **第一步：通过。** 4 轮均值的吞吐差 −0.05%，远小于 5%，逐轮符号交替。最初一对的 +4.05% 触发了补测，补测这一对是 −4.07%。固定速率 p99 由排队决定，两者区间重叠，属于测量噪声。
- **v2（更正后）：通过。** 吞吐差 +0.06%，没有落入 3%–8% 的模糊区间，不需要补测。p99 的情况同上。

## 更正：Rust v2 漏掉了响应头过滤器

**问题。** C v2 在原生响应头过滤器里做两件事（`module/native_phases.c` 的 `ac_header_fields`）：写 `Server: APISIX/3.19.0`；上游状态为 5xx（或开启 `show_upstream_status_in_response_header`）时写 `X-APISIX-Upstream-Status`。首批实测的 Rust v2 没有移植这个过滤器。原有的一致性套件只比较状态码、`WWW-Authenticate` 和上游回显，没有覆盖这两个响应头，所以当时没有发现。

**证据。** 用首批实测的 Rust v2 源码重新构建出的模块，SHA-256 与首批实测的模块完全相同（`d3674278…`）。它返回 `Server: openresty`，上游 502 时没有 `X-APISIX-Upstream-Status`；C v2 返回 `APISIX/3.19.0` 和 `502`（[`old-rust-v2-headers.json`](results/validation-step1-20261109/old-rust-v2-headers.json)）。

**影响。** 功能上，这是一处真实的不一致。性能上，首批测量里 Rust v2 每个响应少做了一次响应头链表写入和一次 `upstream_status` 变量查找，对 C 不公平；"Rust 吞吐 +2.18%、CPU/请求 −2%"可能有一部分来自这里。

**处理。**
- 新增 `module/src/filters.rs`，两种模式共用。v2 直接写响应头链表并设置 `headers_out.server`，和 C v2 一样；第一步用 ngx_lua 的响应头设置函数，和 C 第一步一样。
- 一致性测试新增响应头比较，五条路径全部一致（见上文）。
- 上面性能表里的 C v2 和 Rust v2 是修正后在同一批次重测的结果，替代首批报告中的 v2 数字。

## 边界

- 第一步两条路径各 4 轮，其余路径各 2 轮；4 vCPU 共享云 VM；单路由、2 个消费者、单 worker。不构成统计显著性。
- 共享 VM 的批次间波动很大，绝对数值只能在同一批次内比较。本批的绝对吞吐比首批（`bench-20261109T010224Z`）低约 5%：Lua 16,181 对 16,980，C v2 35,552 对 37,293。
- 第一步只覆盖 key-auth。C 的 V1 适配层是通用的，Rust 只实现了 key-auth 需要的 SDK 子集；要运行更多 Lua 插件，Rust 适配层还要继续扩充。
- Rust 第一步有几处每请求的小堆分配没有优化（keepalive 池 key、scheme 复制、请求头缓存 key），C 用的是栈缓冲。估计每请求 0.2–0.3 µs，对 Rust 不利。
- R_low = 13,300 RPS 对第一步两条路径和原版 Lua 都接近饱和（见晚发比例），这一档的 p99 主要由排队决定，不能当作 SLO 结论。
- 固定速率工具是仓库自带的 `tools/fixed_rate.c`，按 wrk2 的方式从预定发送时刻计算延迟。
