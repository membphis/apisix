# 仅 limit-count：版本、工具与复现

本轮按用户要求取消全局 Prometheus，只启用路由 limit-count，原版/C 各三次 60 秒样本。
沿用上一轮的历史 v6 C 二进制与原版源码，不使用本分支后来合入的候选优化。

- 冻结来源：`a3770f02b3adc2c295e993b8234da3e49f26864c`，其中 APISIX 原码基线为 `b31e828c6f6b588ae954bf69c8432c08fb73bb7e`。
- 模块 SHA-256：`6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8`，模块文件在上一级 `module.so`。
- 用 `git archive` 导出冻结版本的 apisix/conf/bin、模块、桥接、example、原 sampler/prepare/HTTP helper 和参考环境；在现有容器独立目录中展开。
- `harness/` 保存实际用到的 `demo.py`、`benchmark.py`、`recheck_limit_only_v6.py`、`bench.lua`、`test_demo.py`、`runtime.lua`。
  prepare/sampler/runner/bridge 的字节分别与 environment.json 中记录的 SHA-256 一致。
- `reference-environment.json` 保存原模块/源码/插件的参考哈希；测量前逐文件检查核心源码，并在结束时再次检查模块未变。

## 与带 Prometheus 的上一轮相比改变了什么

1. 启用列表从 `prometheus, limit-count` 变为仅 `limit-count`，全局规则为空，消费者为空，key-auth/AI 均未启用。
2. 准备器在不启用 Prometheus 时显式设置 exporter=false，生成配置没有 9091 listener，实际 TCP 连接也被拒绝。
3. 原 sampler 的 metrics 回调替换成 no-op；保留每秒的进程/CPU/RSS 检查、相同报文、CPU 绑定与其余采样行为。
4. 继续保留四个 URI 模式，仅 `/limit/*` 有 limit-count，其他路由插件为空；保持原匹配结构。

没有修改任何 Lua 插件或 C 业务代码，没有重编译模块。
旧 prepare 的启动横幅仍硬编码“global Prometheus :9091”；原始 runner.log 保留这行文字。
它不反映实际配置，逐样本 configuration-check.json、完整配置以及端口断言确认 Prometheus 未启用。
environment.json 的 `reference_limit_qps` 是旧 sampler 携带的带 Prometheus 单样本参考字段，不参与本次配对收益计算。

## 复现

在独立 checkout/worktree 使用 `a3770f02`，按该提交 README 准备相同的固定运行时。
将本档案的 `harness/demo.py`、`benchmark.py`、`recheck_limit_only_v6.py`、`bench.lua` 放到该 checkout 的 `experimental/native-http-core/tools/`；
HTTP helper 和 bridge 维持该提交版本，核对归档模块/源码哈希后，在容器里以新的输出目录运行 runner。
需要的参数为 `--source-commit a3770f02b3adc2c295e993b8234da3e49f26864c --output <新的绝对路径>`。
它会校验原二进制与源码哈希，拒绝将后来候选代码混入本场景。
用当前分支的 `tools/report_limit_only_v6.py <归档目录>` 可独立验证并重建汇总。

## 结果与解释边界

三轮提升 +14.28%、+13.94%、+10.28%，均值之比 +12.81%，CPU/请求合计口径下降 11.38%，六样本错误率为 0。
原版平均 19,741.29 QPS，C 平均 22,269.38 QPS；原版/C 均比上一带 Prometheus 场景的绝对 QPS 高。
上一场景三轮均值之比为 +12.16%，与本轮相差约 0.65 个百分点；共享云环境三轮样本不能证明这点差异具有统计显著性。
这两份档案都不用于验证分支后续候选优化的收益，也不证明长期稳定性。

完成后恢复仅 limit-count 的 native 演示配置，额度 5/60 秒，9091 关闭。
