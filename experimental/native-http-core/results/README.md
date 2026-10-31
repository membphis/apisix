# 一次性研究证据归档

这份目录随 `apisix-next-v4-llm` 分支提交，保存 2026-10-31 交接时已有的实测与验证证据。
后续 `tools/run.py bench` 会覆盖当前轮结果，开始新实验前请先归档。

| 路径 | 含义 |
| --- | --- |
| 根目录 `results.json`、`environment.json`、`module.so` | 最终一分钟矩阵、环境/源码/依赖哈希与实际测量模块 |
| `*-nginx.conf`、`*-resources.json`、`*.txt` | 各样本实际配置与原始 wrk 输出 |
| `verification-optimized.log`、`verification-sanitizers.log` | 最终源码的完整可运行本地回归与模块内存检查 |
| `benchmark-final.log` | 最终八样本的 runner 输出 |
| `before-optimization/` | 初始完整 C 场景，仍有显著负收益；含旧轮 LuaJIT 采样 |
| `before-native-access/` | 桥接/合并缓存后的负收益；含当时模块与 PC/LuaJIT 剖析 |
| `before-native-filters/` | 直接 C API、状态转换和元数据缓存后的完整矩阵；含源码快照与模块 |
| `before-log-prefetch/` | C filters/JSON 后、日志批量准备前的完整矩阵；含源码快照与模块 |
| `profile-functions-before-metadata-cache.log` | 开发函数剖析，用于定位调用和 C 子树热点，不是验收 CPU 占比 |
| `apisix-native-*-sample.json` | 开发过程中个别一分钟样本，不能替代最终配对矩阵 |
| `EVIDENCE_MANIFEST.json` | 两份实验 results 目录的逐文件大小与 SHA-256；不包含 manifest 自身 |

原局部原型数据在 `../../native-standalone/results/`，不用于证明当前完整场景的收益。
较早轮次没有完整源码快照，不承诺能从该轮源码重新构建；当前源码、最终模块和最终环境哈希已对齐。

`.so` 是本研究的小型固定环境证据，不替代 README 的源码构建步骤。
没有保存 Docker 镜像、运行时依赖缓存或宿主机凭据。
原始渲染配置与 profiler 输出保留工具生成的空白字节，由本目录 `.gitattributes` 排除其空白格式检查；源码和文档仍按正常规则检查。
单次样本和模块 sanitizer 通过均不证明统计显著性、长期无泄漏或生产稳定性。
