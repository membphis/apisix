# APISIX 插件零修改：standalone HTTP 核心原生化

> 交接副本：2026-10-31 保存到 `membphis/apisix` 的 `apisix-next-v4-llm` 分支。
> 当前交接入口是 [HANDOFF_TEMP.md](HANDOFF_TEMP.md)。用户已授权本次提交与推送；下文旧分支名、绝对工作路径、权限说明和原全量迁移合同是历史上下文，以顶部修订范围及交接说明为准。

Status: Revised scenario implementation and verification complete; latest 60-second matrix records positive throughput and CPU results

日期：2026-10-30
基线：membphis/apisix `b31e828c6f6b588ae954bf69c8432c08fb73bb7e`
工作区：`/workspace/apisix-native-demo`，分支 `feat/native-standalone-demo`

## 当前有效范围：用户后续修订

用户明确补充：

> 跟这次请求要覆盖的场景有直接关联关系、用得到的功能，才需要实现。其他的只需要占位就好；甚至占位都不需要，文档记录也行。
> 保证我们这次测试场景用到的一些能力，保证它行为正确。剩下的只要骨架正确，该有的都有就行。

本节取代后文关于未使用能力也必须全部实现的旧要求。目标仍是插件零修改、场景所用核心机制由 ngx C module 负责。

- 必须实现并验证 standalone HTTP 初始化/worker 生命周期、请求上下文、路由、全局/路由插件调度、Consumer 认证和配置合并、请求/响应、单个静态 HTTP 上游代理及相应错误与清理。
- 验证原版 Lua key-auth、local limit-count 和全局 Prometheus；认证和限流只在指定前缀路由启用。覆盖各自与组合场景、认证失败、限流拒绝、404、上游错误/超时、共享配额和指标正确性。
- 未使用的健康检查、慢启动、服务发现及其余插件/SDK 特性不要求实现。保留合理扩展点，文档明确列出未实现能力；不使用空实现冒充等价支持。
- 当前已实现的扩展路由、脚本、DNS、热更新等保留其回归，但不继续扩大本次验收范围。
- native 运行时禁止加载原版 Lua 核心业务模块；Lua 插件、协程传输桥接、NGINX/OpenResty 和第三方运行时原语继续使用。
- 先用同一源码、依赖、配置和资源验证场景行为，再跑 4 条场景 × Lua/native 两种实现，每个样本 60 秒；报告 QPS、CPU/请求、p99、RSS 和错误，不设未经用户要求的提升门槛。

后文的全量职责清单保留为架构参考，未使用功能不再阻塞本次 demo 交付。

本次交付已完成修订场景的实现、差分回归和模块 ASan/UBSan 检查。2026-10-31 最新 8 个一分钟样本中，C 吞吐比原版高 1.4%–11.1%，CPU/请求低 1.3%–10.0%，结束 worker RSS 低 33.8%–35.6%；四组吞吐变化依次为基础 +9.9%、认证 +1.4%、限流 +11.1%、组合 +9.2%，错误率均为 0。共享云环境的单次样本不提供统计置信区间，尤其认证场景的小差距不能据此确认稳定收益，也不据此声称长期稳定性优于原版。
此前负收益矩阵完整保留。后续优化直接调用既有运行时导出的 C API，并采用无 Lua 回调配置的原生过滤链、C 固定窗口诊断 JSON 编码、配置代际缓存和日志变量批量准备；Lua 插件源码未修改。早期极简原型省略部分核心职责，其 40%–50% 提升不作为本次完整场景实现的理论上限或验收结果。
场景职责和未实现边界见 `/workspace/apisix-native-demo/experimental/native-http-core/SCENARIOS.md`，完整实测与复现入口见同目录 `RESULTS.md` 和 `README.md`。后文“完整迁移合同”及旧差距是历史架构参考，不代表仍需实现所有未使用能力。

## 目标与确认记录

用户最初请求将 APISIX 核心改为 ngx module，插件继续保留 Lua，验证 limit-count、Prometheus、key-auth，探索性能和稳定性。之后明确：

> apisix 目录下的不同阶段逻辑代码，以及路由匹配逻辑等，都应有 ngx c module 的完整重写逻辑

> 我的目标，是所有插件之下的 apisix 核心代码逻辑，都用 ngx c 方式重写

> 探索的是，插件不改的情况，底层机制优化

用户已选择先完整重写 standalone HTTP 数据面。本规格据此纠正之前助手自行缩减的范围，不再把三个插件或四条前缀当作底层实现边界。当前旧原型不是目标交付。

## 保持不变的业务边界

- 现有 Lua 插件业务源码不修改，含插件目录中的辅助模块；兼容现有 require 路径和调用方式。
- 首轮场景：全局 Prometheus，key-auth 和 local limit-count 配置在指定前缀路由。
- 每个压力测试样本采集 60 秒，不擅自添加 30 分钟持续压测或多轮矩阵。
- 配置模式先采用 standalone HTTP，不引入 etcd 控制面；TLS/stream 协议处理不纳入本轮。
- 三个插件是验证起点；核心框架及 SDK 的兼容目标覆盖现有 Lua 插件，不允许为三者写特例调度。

## 完整迁移合同

1. 所有 HTTP 请求阶段的 APISIX 核心处理有实际 C 实现：初始化、worker 生命周期、请求准备、插件 rewrite/access、before_proxy、balancer、header_filter、body_filter/延迟过滤、log、退出和清理。不能删掉源逻辑、留空阶段或调用旧 Lua 核心代替重写。
2. 路由在 C 中实现原有 HTTP 匹配机制与语义：精确/前缀/参数 URI、host、methods、remote_addr、表达式、优先级、捕获和无匹配处理。不能用仅支持几个前缀的线性扫描替代完整路由能力后宣称完成。
3. 插件框架在 C 中实现加载/注册、schema/metadata、priority、自定义优先级、enable/disable、筛选与执行计划、全局/路由规则、prefer_route、错误返回、提前退出及生命周期调度。Lua 可以保留 ngx_lua 协程执行所需的薄跳板，但核心决策和状态机必须在 C。
4. 原有 Route、Service、PluginConfig、Consumer、ConsumerGroup 和全局规则的索引、继承合并、优先级、版本和缓存失效规则在 C 中实现。身份绑定后的重新合并和 rewrite 行为必须保持。
5. 上游准备、引用/内联配置、主机/URI/头处理、选点、权重/优先级、超时、重试、健康状态/故障反馈及清理由 C 管理；已有 NGINX 原生传输/事件机制继续复用。不能把所有路由固定到单个上游来替代这些逻辑。
6. 插件使用的 APISIX 模块及 API 在原路径下提供兼容的 C 实现或纯适配外观，包括 apisix.core、plugin、consumer、router、upstream、resource、secret、timer/event、日志/批处理和相关工具。不能保留这些模块的 Lua 业务实现后称其“插件兼容层”而跳过迁移。
7. Lua table、函数参数/返回值、对象身份/可变性、nil/JSON null、数组 metatable、ctx.var、配置元数据、资源 key、共享字典/配额语义、缓存版本及错误行为须保持。语言边界转换不能改变插件行为。
8. 插件本身的 Lua 逻辑与 OpenResty/LuaJIT/ngx_lua 的运行时机制继续使用。第三方依赖与 NGINX 原生复用逐项列清；自定义 Lua 用户扩展属于回调代码，调度决策仍由 C 管理。

“核心代码使用 C”约束逻辑归属，不要求强行把插件可见的 Lua table 改为不兼容 userdata，也不以源码行数作为验收指标。

## 已知事实与当前差距

旧实验 C 文件只有 267 行（不计空行/注释 230 行），仅有前缀匹配、请求指针、位掩码和固定上游名称。阶段数组仍由 Lua 建立，使用旧 plugin.run_plugin 和 core.ctx/consumer，完整 Lua 初始化继续运行，header/body 核心处理被跳过。它未达到本规格，旧性能数据仅对局部实验有效。

静态盘点检查了插件目录 224 个 Lua 源文件（含辅助模块），发现 43 个直接引用的非插件 APISIX 模块、84 个字面量依赖闭包模块，以及 137 种 core SDK 成员访问路径。此扫描不是 Lua AST 分析，无法穷尽动态 require、别名和成员访问，不能据此宣称 API 面已完整。

源职责到迁移要求的清单保存在 `experimental/native-standalone/COVERAGE.md`，扫描证据在 `CORE_API_INVENTORY.json`。相关请求核心主要文件共 7441 行（包含注释/非 HTTP 分支），只是规模参考。

## 验收与证据

- 每项 APISIX HTTP 核心源码职责映射到具体 C 符号和测试；列明 NGINX 复用和本轮协议排除项，不使用空函数/包装旧核心凑覆盖。
- 原生启动和请求不加载旧 APISIX Lua 核心业务实现；验证应禁止整类旧实现或检查实际加载路径，而不只是把四个入口替换为报错函数。
- 使用阶段探针插件验证全局/路由优先级、rewrite/access/before_proxy/header/body/log、提前退出、错误、流式多 buffer、内部重定向及合法 yield/恢复；探针只用于回归，不修改真实插件。
- 对原版基线做路由、配置合并、身份绑定、上游与错误路径的行为差分，覆盖分支而非仅四条前缀。
- 原 key-auth、limit-count、Prometheus 的源码保持不变，分别及组合验证实际 HTTP 行为和指标计数；SDK 用例覆盖更广的原插件依赖面，并明确未验证项。
- 原生内存检查与生命周期/故障回归通过。按最终实现重新跑 60 秒样本对照，记录有效 QPS、CPU/请求、RSS、错误和延迟测量口径。保留同环境公平对照，不设未获用户要求的提升百分比。
- 完成必须建立在迁移覆盖和兼容证据之上，不能仅靠 LOC、demo 可运行、少数入口未被调用或局部 QPS 上升。

## 当前状态与执行权限

目标和 HTTP 范围已由用户确认。原实现请求有效，常规源码调查、隔离实现、依赖准备和验证不需要再次开工确认。若出现需要改变插件接口/语义、协议范围或用户目标的决策，再回讨论澄清。

本次修订场景的实现和验证已完成；APISIX 全量核心与所有插件兼容性不属于本次完成声明。旧实验与历次测量保留并标明范围。暂不提交、推送、创建 PR、合并或发布。严禁把局部原型报告为完整底层优化结果。
