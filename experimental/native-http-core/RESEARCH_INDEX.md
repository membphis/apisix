# 研究入口：C 插件直连、C/Lua 混用与逐步迁移

记录日期：2026-10-31。本文汇总已提交的实现和证据，方便不熟悉对话背景的评审者阅读。
新的演进设计见 [NATIVE_PLUGIN_MIGRATION.md](NATIVE_PLUGIN_MIGRATION.md)。

目前得到的工程结论是：在已验证的 standalone HTTP 范围内，可以让 C 核心与 C 插件直接
协作，也可以保留未迁移的 Lua Prometheus；不必先把所有插件一起重写。插件逐个迁移因此
有了一条可实验、可验证、可回退的路径。未来是否继续迁移某个插件，仍由它的实际成本、
语义完整性和维护代价决定。

## 阅读顺序

1. 本文：分支、阶段、同功能结果和证据入口。
2. [渐进迁移设计](NATIVE_PLUGIN_MIGRATION.md)：两种请求路径、源码衔接、能力边界、回退和后续接口设计。
3. [v2 原生 key-auth 专项](NATIVE_KEY_AUTH_V2.md)：业务移植与请求协作方式的四路径对照。
4. [v3 混合 Prometheus 专项](HYBRID_PROMETHEUS_20261031.md)：保留原版 Lua 指标的正确性、吞吐、延迟和成本。
5. [v1 完整优化记录](EXHAUSTIVE_20261031.md)、[早期单插件测量](EXPERIMENTS_20261031.md)：
   插件不改时的结果、负收益、已有优化和剩余不确定性。

这些文档描述不同实验边界。历史报告及原始证据保持原样；本文不替换它们的测量结论。

## 分支与 PR 的组织

| 阶段 | 分支 | 实现/归档检查点 | 内容 |
| --- | --- | --- | --- |
| v1 | `apisix-next-v4-llm-core-v1` | [`b8392320`](https://github.com/membphis/apisix/commit/b839232031a3797a1ddcee9b20f8a81f4ef0bf72) | C 核心调用未修改的 Lua 插件；完整正负证据 |
| v2 | `apisix-next-v4-llm-native-plugins-v2` | [`caf19a14`](https://github.com/membphis/apisix/commit/caf19a144cc6f9caedcf875a2649f7e30eb78b7e) | C key-auth、旧适配器对照、完整 C 请求路径 |
| v3 | `apisix-next-v4-llm-native-plugins-v3-hybrid` | [`0afeb689`](https://github.com/membphis/apisix/commit/0afeb689cd007f5f9800785c254f4e1dbaec2c89) | 同一 worker 中 C 请求路径＋未修改的 Lua Prometheus |

本专项研究 PR 以 v1 分支为比较基线，head 使用 v3 分支，因此包含 v2、v3 实现及后续说明文档。
这是固定检查点上的增量研究评审，不是向 APISIX 官方主干提交的生产替换方案，也不执行合并。
后续对本专项的文档完善和小步改动继续追加 commit；新的实验另建证据目录，不覆盖旧样本。

原始 `apisix-next-v4-llm` 分支另有并发研究。整理时它已推进到
[`8b8dac1e`](https://github.com/membphis/apisix/commit/8b8dac1ea923331d55e8d02763b21d456ada0eda)，
包含独立限流测量及核心 CPU/GC/固定负载诊断。本专项没有合并或覆盖该分支；它的结果不能
当作当前 v3 二进制的新测量。未来整合时需要单独处理源码差异并重新验证。

## v2：只启用 key-auth，区分业务和协作方式

四条路径均为 route-only key-auth，没有 Prometheus、AI 或其他插件。相同有效密钥、隐藏
凭据和真实上游代理，正序、逆序各一轮，每个压力样本 60 秒。

| 路径 | 两轮平均 QPS | 对照用途 |
| --- | ---: | --- |
| 稳定 Lua 核心＋Lua key-auth | 23,532.01 | 同功能基线 |
| C 核心＋Lua key-auth | 25,606.25 | 仅迁移核心 |
| C 核心＋C key-auth，保留旧 Lua 阶段适配 | 28,601.99 | 业务移植后的中间态 |
| C 核心与 C key-auth 直接协作 | 46,738.93 | 业务请求热路径不执行 Lua |

直接协作相对稳定 Lua 为 **+98.62%**，相对 C 核心＋Lua key-auth 为 **+82.53%**。
单独移植认证业务相对后者为 +11.70%；同一份 C 认证去掉旧阶段适配后，相对 C 适配器为
+63.41%。这些相对不同分母的比例不能相加。

这一对照支持继续优化插件与核心之间的状态和阶段协作。它不证明一个 Lua 函数调用或
Lua VM 本身占了固定比例的时间：改变还包括上下文表、registry、SDK 转换、变量发布、
路由准备和 balancer 衔接。Lua VM 始终按 worker 存在，并非每个请求新建一台 VM。

证据：[v2 汇总](results/native-key-auth-v2-20261031/paired-summary.json)、
[清单](results/native-key-auth-v2-20261031/MANIFEST.json)、
[诊断](results/native-key-auth-v2-20261031/diagnostic-summary.json)。

## v3：同一 worker 中保留 Lua Prometheus

三条带指标路径使用相同业务资源、相同全局 Prometheus 和相同 1 Hz 抓取。另有无指标 C
参考路径用于衡量指标代价。四路径正序、逆序各一轮，每个压力样本仍为 60 秒。

| 路径 | 两轮平均 QPS | 平均延迟 ms | P95 ms | P99 ms |
| --- | ---: | ---: | ---: | ---: |
| 稳定 Lua 核心＋Lua key-auth＋Lua Prometheus | 16,768.03 | 1.958 | 3.049 | 4.037 |
| C key-auth＋旧阶段适配＋Lua Prometheus | 19,749.60 | 1.628 | 2.146 | 2.604 |
| C 路由/认证/peer＋仅 log 阶段进入 Lua Prometheus | 24,294.76 | 1.320 | 1.782 | 2.183 |

混合路径相对同功能 Lua 基线平均 QPS **+44.89%**，两轮分别 +45.70% / +44.07%；
平均延迟、P95、P99 的逐轮统计均值分别下降 32.60%、41.56%、45.92%。
相对保留旧阶段适配的 C 认证，混合路径又提高 23.01%。

无指标 C 参考的平均值为 47,623.56 QPS；加入 Lua Prometheus 后吞吐下降 **48.99%**。
这个成本必须与收益一起保留。它包含指标逻辑、SDK 视图、桥接、分配和导出工作，不能全部
归给跨语言调用。无指标参考与带指标路径功能不同，不能作为同功能加速比。

P99.9 有明显波动：Lua 基线两轮为 5.684 / 44.761 ms，混合版为 3.981 / 3.453 ms，
无指标 C 为 35.268 / 2.641 ms。原值及未确定的原因完整保留，不宣称稳定的 P99.9 收益。

证据：[v3 汇总](results/hybrid-prometheus-20261031/paired-summary.json)、
[清单](results/hybrid-prometheus-20261031/MANIFEST.json)、
[诊断](results/hybrid-prometheus-20261031/diagnostic-summary.json)、
[完整性检查](results/hybrid-prometheus-20261031/final-integrity.json)。

## 正确性和兼容范围

v2 对四路径分别验证 47 个业务场景，另测并发、keepalive、拒绝请求不进入上游，以及 C 路径
的在途配置代际、密钥轮换和不支持配置的拒绝/恢复。
v3 对三条带指标路径分别验证 50 个场景，另测 128 并发、499、配置代际和双 worker 聚合；
指标标签、计数、直方图以及访问日志实际收发字节均使用精确断言。额外 getter 标签验证了 SDK
缓存。模块 ASan/UBSan 和默认路径的六配置核心回归（1,967 项）通过。

“通过”仅指报告列出的检查。运行时及其他 DSO 没有一起加 sanitizer，泄漏检查关闭；
没有完成长期运行、多硬件或全部 APISIX 功能验证。当前 C 请求闭环只支持明确限定的
静态 HTTP、literal key-auth 配置；v3 扩展到 log-only Prometheus，并非任意 Lua 插件混用。

请求热路径为 C，不等于移除 LuaJIT：配置加载、校验、初始化、timer，以及 v3 的指标回调仍使用 Lua。
原版 key-auth 和 Prometheus Lua 源码没有修改；C key-auth 是显式选择的另一实现。
其他 Lua 阶段、动态配置对象写入、secret provider、多认证、健康检查/发现等边界见两份专项报告。

## 证据保留与重现

稳定基线固定为 APISIX **3.19.0**、commit
`473043623c161966f3f6ef4e8f12d2d8667a12d2`。两边使用共同的
OpenResty 1.27.1.2 / APISIX runtime 1.3.2 和固定依赖；其中 API7 Prometheus 库为 1.1.0，
release rockspec 声明为 1.0.0。因此这是共同运行时上的源码对照，尚非原装发行镜像对照。

网关单 worker 固定 CPU0，上游 CPU1，wrk t2/c32 固定 CPU2–4；父级四核配额，3 字节
上游响应，每样本 5 秒预热＋60 秒压力。正式 v2 和 v3 各有 8 个样本，未按吞吐挑选结果。
两轮不能证明统计显著性；分位数均值不是合并分位数；闭环吞吐对应的延迟不能替代固定 RPS
下的 SLO 曲线。当前测量也不能证明任何实现已达到性能极限。

所有实验失败、负收益、测试 fixture 修正、配置、依赖、源码/二进制哈希和原始输出保留在各自
目录。v3 归档时核对了 1,404 条历史文件记录，新增清单包含 229 份证据；被 Git 默认忽略的
原始 `.log` 也已纳入提交。阶段 `REPORT.md` 是归档副本，新增说明不会回写这些旧清单。

在 [README](README.md) 准备好的固定容器中，分别使用新的输出目录，依次运行：

```sh
export APISIX_KEY_AUTH_OUT="/tmp/native-key-auth-repro-$(date -u +%Y%m%dT%H%M%SZ)"
python3 /src/experimental/native-http-core/tools/key_auth_experiment.py validate
python3 /src/experimental/native-http-core/tools/key_auth_experiment.py bench
python3 /src/experimental/native-http-core/tools/key_auth_experiment.py diagnostics

export APISIX_HYBRID_OUT="/tmp/hybrid-prometheus-repro-$(date -u +%Y%m%dT%H%M%SZ)/final"
python3 /src/experimental/native-http-core/tools/hybrid_experiment.py validate
python3 /src/experimental/native-http-core/tools/hybrid_experiment.py bench
python3 /src/experimental/native-http-core/tools/hybrid_experiment.py diagnostics
```

工具拒绝覆盖已有 label；验证、构建、诊断与压力不要并发。当前工具重现当前源码的方法，
精确复核历史运行应使用各归档内的 module-source、runtime 和 harness 快照。

## 后续记录规则

后续优先完善可复用的 C 插件编译/执行契约，并先获得固定 RPS 延迟、更多路由/消费者和
Lua Prometheus 的 JIT/GC 成本证据。每新增一种插件或混用阶段，先写明语义、能力和拒绝条件，
再加入差分验证与 60 秒配对测量。C 迁移是由证据推动的演进方向，不预先承诺每个插件的收益，
也不要求低成本 Lua 插件为追求语言统一而重写。
