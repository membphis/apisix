# V1 技术预研里程碑

用户将本轮工作保存为 V1 优化里程碑，供另一个会话从不同视角继续研究。
两个视角的技术预研共同为后续方案设计提供依据；本次冻结不代表最终架构选择或生产版本验收。
这里的 V1 是研究迭代编号，`next-v4` 是研究项目名称，不是 APISIX 官方版本声明。

## 固定入口

- 仓库：`membphis/apisix`。
- V1 冻结分支：`apisix-next-v4-llm-v1`。本次交付后不再在该分支继续优化；新会话从最终交付的完整 commit SHA 派生新分支。
- 原临时分支 `apisix-next-v4-llm` 保持在 `3e63b632ca33b805d42fb839738cf8120aa11ead`，未并入本次收尾文档。
- V1 继承该提交的全部实现与实验历史，本次仅补保存核验、讨论记录、文档入口与交接提示词。
- [保存核验清单](milestones/v1/PRESERVATION_AUDIT.json)记录被核验的源码提交、文件 SHA-256、历史可达性及容器源码对应关系。

请先读[双方讨论与结论记录](V1_DISCUSSION_RECORD.md)，再读[下一会话提示词](NEXT_MODEL_PROMPT.md)。
源码、实验观测、用户确认的范围和研究假设分别记录，不能互相替代。

## 已保存的实现

完整实现位于 [module/](module/)，共 **24 个 C 文件、10,348 行 C 源码**（含注释和空行），
连同3个头文件及构建配置，共28个模块文件。[runtime.lua](runtime.lua)为101行必要运行时桥接。
以上为冻结源码计数；旧交接中9,388行/68行的数字属于历史 v6，保留其历史含义。

| 保存对象 | 路径或版本 | 含义 |
| --- | --- | --- |
| 当前完整核心及所有候选开关 | `module/`、`runtime.lua` | 与最终 `candidate-module-source/` 的28个文件及 `candidate-runtime.lua` 逐字节一致 |
| 构建、场景、测试与诊断工具 | `Dockerfile`、`example.json`、`tools/`、`tests/` | 连同实际配置和测试日志保存；不只保存二进制 |
| 早期局部原型 | [native-standalone](../native-standalone/) | 包含267行C模块及当时Lua桥接/测量，只作为历史预研 |
| 历史完整 v6 | 提交 `a3770f02b3adc2c295e993b8234da3e49f26864c` | 对应全局指标矩阵、两类三轮限流复测与后续专项使用的冻结核心 |
| 各阶段优化与筛选版本 | `results/before-native-filters/`、`before-log-prefetch/`、`single-20261031/`、`exhaustive-20261031/` | 保留已有源码、模块、配置、正负结果；首批筛选与最终候选有独立快照 |
| 追加/未默认启用的修改 | `results/exhaustive-20261031/staged-patches/` 及当前模块的实验开关 | 保留补丁、试验实现、共享正确性修复及边界说明 |
| 核心计时/GC诊断版本 | `results/core-cost-v6-20261031T0757Z/diagnostic-module-source/`、`source-combined/`、`source-separated/` | 诊断源码完整留档，不把诊断构建作为正常性能构建 |
| 减法及插件放大 | `tools/subtraction/`、`results/subtraction-v6-20261031T0925Z/` | 保存方法、原始样本、实际临时插件副本和限制；未改变正常核心实现 |

早期某些负收益轮次没有当时完整源码快照，已有报告明确说明这一点；保留其测量与优化过程记录，
不声称每一个历史中间状态均可逐字重建。当前仍存在的模块源码、补丁、工具和可追溯修改均已保存。

## 保存核验

本次核验的是保存完整性，没有重新编译或重新压测。

- 原临时工作树及原始 `apisix` 工作树均干净；完整模块目录没有未跟踪源码，忽略项仅有 Python 缓存。
- 当前28个模块文件、桥接与归档最终候选逐文件一致。
- 两个已知实验容器中检查到的85份模块源码/配置文件，全部匹配已提交文件或可达历史提交中的内容。
  检查目录与匹配路径逐项保存在核验 JSON，不把该检查扩大为整个容器文件系统审计。
- 原证据清单的 **1,518 份文件**全部通过大小与 SHA-256 核验；历史实现、候选、失败记录和诊断记录均保留。
- 原 APISIX `apisix/` 目录与本项目最初源码基线一致，插件业务源文件未改。放大实验只修改临时运行副本，副本亦留档。
- 关键实现和证据提交都在 V1 祖先历史中，无需依赖本地 reflog、运行中的容器或未推送提交获取它们。

核验 JSON 的文件条目描述源码审计提交 `3e63b632`，含 Git blob ID，可以从Git对象直接取回对应字节；
本次追加的入口文档由最终V1提交保存，避免清单自引用。干净克隆后，可先检查核心没有被收尾更改：

```sh
git diff 3e63b632ca33b805d42fb839738cf8120aa11ead HEAD -- experimental/native-http-core/module experimental/native-http-core/runtime.lua experimental/native-standalone
git log --oneline b31e828c6f6b588ae954bf69c8432c08fb73bb7e..HEAD
```

第一条应无输出；完整条目可用 `git cat-file blob <git_blob>` 取回并核对 SHA-256。

## 版本与性能证据的边界

| 版本/场景 | 观测 | 对应记录 |
| --- | --- | --- |
| 历史 v6，全局 Prometheus + 路由 limit-count | 原矩阵 +11.1%；三轮复测均值之比 +12.16% | [原报告](RESULTS.md)、[三轮复测](results/limit-recheck-20261031T0617Z/REPORT.md) |
| 同一历史 v6，仅路由 limit-count | 三轮均值之比 +12.81%，worker CPU/请求下降11.38% | [仅限流报告](results/limit-only-v6-20261031T0644Z/REPORT.md) |
| 后续稳定源、O3六开关候选 | 两轮均值：Prometheus +7.80%、key-auth +3.97%、local limit-count +6.81% | [候选报告](EXHAUSTIVE_20261031.md) |
| 历史 v6核心专项 | 计时探针影响两版执行，未取得可信纯核心净倍率 | [专项报告](results/core-cost-v6-20261031T0757Z/REPORT.md) |
| 历史 v6减法/放大 | 原插件总worker CPU下降10.51%；重复路径斜率存在差异，但基线漂移影响归因 | [减法报告](results/subtraction-v6-20261031T0925Z/REPORT.md) |

上述版本、Lua基线、运行时、硬件条件和插件布局不同，不能拼成同一组前后比较。
最后的减法结论未获得用户对其解释的认可；用户本次要求保存预研，留待另一视角共同判断。
没有把任何纯核心比例、GC归因、长期稳定性或绝对性能上限作为 V1 已确认结论。

当前实现保留全部候选，实验开关默认关闭。后续候选最终测量使用 `-O3` 及以下六项：

```nginx
apisix_core_lazy_rate_json on;
apisix_core_batch_vars on;
apisix_core_lazy_route_opts on;
apisix_core_request_header_flags on;
apisix_core_header_scratch on;
apisix_core_request_pool_native on;
```

精确构建元数据、模块哈希及配置见 `results/exhaustive-20261031/selected-*`。
源码中保留某个开关，不代表它默认启用或已证明有收益。

## 后续会话的工作方式

1. 用最终交付的 V1 commit SHA 建立新工作分支，保留 V1 和本轮原始证据。
2. 从[讨论记录](V1_DISCUSSION_RECORD.md)恢复用户目标，再核对实际代码、版本和场景。
3. 可以挑战 V1 的接口划分、状态所有权、语言边界和测量方法，从另一个角度提出方案；无需机械延续已有16项微优化。
4. 对不同设计列出机制、兼容约束、成本假设及最小验证，再与 V1 同条件比较，保留负结果。
5. 最后结合两个视角的预研，由用户决定方案设计；不预先把任一预研实现定为最终架构。

已有构建与验证步骤见 [README](README.md)，场景边界见 [SCENARIOS](SCENARIOS.md)，
详细优化链条见 [HANDOFF_TEMP](HANDOFF_TEMP.md) 与 [EXHAUSTIVE_20261031](EXHAUSTIVE_20261031.md)。
