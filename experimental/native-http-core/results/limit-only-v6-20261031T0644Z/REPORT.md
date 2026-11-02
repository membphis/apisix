# 原 v6 核心：仅 limit-count，关闭全局 Prometheus

[版本与复现](PROVENANCE.md)。

日期：2026-10-31T06:44:06.477467+00:00。复测同一历史 C 二进制，未引入后续候选优化。
原版三轮平均 19,741.29 QPS，C 平均 22,269.38 QPS，均值之比变化 **+12.81%**。
逐轮变化范围 +10.28%–+14.28%。六样本错误率均为 0。

| 轮次 | 顺序 | 原版 QPS | C QPS | 变化 | 原版 CPU μs/请求 | C CPU μs/请求 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | lua → native | 19,184.40 | 21,924.14 | +14.28% | 52.08 | 45.58 |
| 2 | native → lua | 19,913.71 | 22,689.71 | +13.94% | 50.19 | 44.03 |
| 3 | lua → native | 20,125.75 | 22,194.30 | +10.28% | 49.63 | 44.98 |

总 CPU 秒/总请求数：原版 50.61 μs/请求、C 44.85 μs/请求，变化 -11.38%。

## 配置与方法

- 双方启用列表严格为 `["limit-count"]`；`global_rules=[]`、`consumers=[]`。没有 Prometheus、key-auth 或 AI 插件。
- `/limit/*` 仅 limit-count；保留上一场景的四个 URI 模式，其他路由插件为空，以控制路由匹配结构。
- Prometheus exporter 显式关闭，9091 不监听且 TCP 连接被拒绝；没有指标抓取。旧准备器的启动横幅写死了“global Prometheus :9091”，该横幅不是实际状态，配置与端口断言才是证据。
- C 模块 SHA-256 为 `6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8`，与上次带全局 Prometheus 的三轮一致。
- 冻结源码来自 `a3770f02b3adc2c295e993b8234da3e49f26864c`；仅准备器增加 exporter=false，runner 清空全局规则/限制插件并将旧采样器的抓取回调置空。
- 旧 sampler 的其余逻辑、报文、每秒进程/CPU/RSS 观测与错误检查保持一致；无效 apikey header 保留，不启用认证。
- local、constant key `demo`、1,000,000,000/600 秒，全部压力请求经过计数后转发到真实后端；限流响应头已预检。
- 单网关 worker CPU 0、上游 CPU 1、wrk CPU 2–4，2 线程/32 连接、HTTP/1.1 keepalive，后端返回 3 字节 `ok`。
- 各样本重启网关，预热 5 秒、正式 60 秒；顺序 Lua→C、C→Lua、Lua→C。六份资源与本地配置逐一比较完全一致。
- worker 替换、非 2xx/3xx、socket error 或新增 error/alert 会让样本失败。完成后恢复仅 limit-count 的 native 配置，额度 5/60 秒。

## 延迟与 RSS

| 轮次 | 实现 | p99 ms | 结束 RSS MiB |
| --- | --- | ---: | ---: |
| 1 | lua | 3.132 | 31.42 |
| 1 | native | 2.308 | 19.21 |
| 2 | native | 2.220 | 19.45 |
| 2 | lua | 3.113 | 31.85 |
| 3 | lua | 2.988 | 32.05 |
| 3 | native | 2.389 | 19.32 |

共享云环境三次配对不提供统计置信区间；闭环 p99 处于各实现各自达到的吞吐，不是固定到达率延迟。
本结果只对应上述历史 v6 二进制/源码，不用于证明分支后续候选代码的性能。
完整配置、预检记录、原始 wrk、environment.json、results.json 和 summary.json 在本目录。
