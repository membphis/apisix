# 减法/插件放大：版本与证据边界

本实验执行用户提出的思路：先用纯 NGINX 减法覆盖原 scope 探针未计入的机制成本，
再重复真实限流路径，检查能否用斜率/截距解释开销。不是新的核心性能优化或吞吐复测。

- 冻结代码：`a3770f02b3adc2c295e993b8234da3e49f26864c`；APISIX Lua 源码基线为
  `b31e828c6f6b588ae954bf69c8432c08fb73bb7e`。
- 原核心模块 SHA-256：`6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8`；
  实际文件保留在[上一层 module.so](../module.so)，本次没有重编译。
- 该模块对应此前全局 Prometheus + limit-count 和仅 limit-count 的历史 v6 复测。
  本分支后续研究使用的核心/编译开关不混入；结果不能用于证明后来版本的收益。
- 运行时沿用已固定的 OpenResty 1.27.1.2 / ngx_lua 0.10.28 / APISIX runtime 1.3.2，
  同一个 `apisix-native-demo` 容器；详细依赖和源文件哈希见 `reference-environment.json`。
- `environment.json` 保存实际 runner、负载器、原插件和模块 SHA、CPU、速率与完整18样本顺序。
  runner 开始时逐文件比对冻结核心源文件哈希，结束时再次比对原模块。

## 工具与请求路径

`harness/` 保存实际 runner、load.py、冻结 demo.py/benchmark.py/test_demo.py、薄 runtime.lua，
以及本次分析器和分析计划。demo.py 来自前一次仅限流实验，显式关闭未启用的 Prometheus exporter。
没有使用 `tools/core_cost/` 内的任何计量库或 wrapper；仅复用其独立 HTTP 负载器。
Startup audit 只在 init_worker 核验启用列表与已加载插件，不在逐请求路径执行。

原插件对照保持源文件不变。放大组仅在临时运行目录把 access 中原 rate_limit 调用重复 k 次；
每次仍实际更新 quota、设置原响应头、调用原 SDK，不能当作重复空函数。
返回拒绝时立即退出；每个外部请求仍恰好代理一次。k=0 为独立空回调，不进入正 k 拟合。
每份 `measured-limit-count.lua` 保存实际使用的插件副本；仓库 `apisix/`、C module、runtime 未改。

全局规则和消费者为空，启用列表为空或仅有 limit-count。无 Prometheus、key-auth、AI；
负载器沿用的 `apikey: demo-key` 只是双方相同请求字节，不会注册或执行认证插件。
四条原前缀路由保留，仅 `/limit/*` 有限流配置。配额十亿/600秒，local policy，正常请求不触发429。
每组保存完整配置、启用列表校验、quota差、上游echo、warmup/load、worker CPU/RSS与error日志。

## 预检与公平性

`preflight-v1/`、`preflight-v2/` 和对应日志保留两次完整功能/容量预检。
第一版发现纯 NGINX 的 `$host` 去掉了 Host 端口，而 APISIX 原样传递；
在任何正式样本开始前改为 `$http_host`，并重跑完整预检。
第二版确认纯 NGINX/Lua 无插件/C 无插件发往上游的 headers 字典完全一致。
这不能消除静态 upstream 与 APISIX 动态 upstream 的内部机制差异，差异被包含在 APISIX 整合成本中。
旧 demo banner 固定写 Prometheus :9091，不代表实际启用；配置、启动插件断言与结束端口核验为准。

每样本60秒，预热5秒；3,000计划请求/秒、32连接、180,000完成请求。
gateway 绑定 CPU0、upstream 绑定 CPU1、负载器绑定 CPU2–4。最重的 k=8 先核验 CPU 余量。
CPU 只在负载窗口前后读取进程计数，普通 GC 一直开启；没有每请求时钟或额外 profiler。
这组固定负载主要比较单位请求消耗和真实延迟，不把相同 QPS 解释为吞吐收益。

## 分析与解释

使用[分析计划](../../tools/subtraction/analysis-plan.json)与[方法](../../tools/subtraction/README.md)。
计划是在矩阵进行中固定、已看到早期观测之后写下，不声称严格事前注册。
前后基线的端点敏感性不是置信区间；正 k 每组仅一个样本。
原插件、循环k=1、空回调分别保留；线性不通过时不输出可信的固定/增量分离。
NGINX减法给出 APISIX 附加整合成本，不是函数self time；插件增量还包含核心SDK与GC，不能全归给插件业务。
正常 GC 已进入 CPU，不能单独确定核心因果GC或GC pause，也不能再加一遍。
Latency 原始分布直接比较，p99不做部件相减。

`REPORT.md`、`analysis.json` 从全部18份结果重新生成。`runner.log` 保留正式顺序和完整输出；
`restoration.json` 记录结束后恢复的原 native limit-only 配置（5/60秒）、插件文件哈希和无9091端口等核验。
