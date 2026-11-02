# 不插请求计时探针：减法与插件执行次数放大

用户提出先比较纯 NGINX 和 APISIX 代理，再放大插件工作量，用差值/拟合估算成本。
这套工具实践该方法，保留无法识别的部分，不把代数恒等式写成因果验证。

## 计量边界

- 主指标为单个 gateway worker 的 `/proc` user+system CPU / 完成请求。
  排除独立 upstream 和负载器进程；包含 worker 内 NGINX、内核执行、APISIX、Lua VM、正常 GC 和后台任务。
- 纯 NGINX 使用相同 OpenResty 二进制、响应体、转发请求头、上游地址和 keepalive 参数，无 APISIX 请求钩子。
  静态 upstream 与 APISIX 的动态适配仍有机制差异，减法包括这部分差异。
- 无插件 APISIX 减纯 NGINX，估计 APISIX 附加/整合成本。它覆盖 VM 入口退出等原 scope 探针遗漏的成本，
  但不是核心函数 self time，更不是启用插件时所有核心 SDK 成本。
- 正常 GC 保留；无逐请求时钟、allocator 拦截、GC profiler 或 SDK wrapper。
  核心造成的独立 GC CPU/pause 无法仅凭减法识别。
- CPU 的计量不计网络等待；Latency 单独记录，p99 不能相减来拆出部件 p99。

## 工作量与控制组

使用冻结 `a3770f02b3adc2c295e993b8234da3e49f26864c` 和原 v6 模块
`6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8`，
对应此前 limit-count-only 三轮吞吐对照。主分支后续优化不混入。

全局规则为空。无插件组的启用列表为空；其他 APISIX 组只启用 `limit-count`，仅 `/limit/*` 路由使用。
无 Prometheus、key-auth 或 AI 插件。保留原四条前缀路由和原插件源码。
循环实验仅改临时运行目录的插件 access，不改仓库插件，用户已授权该实验性工作量修改。

一次外部请求仍只代理一次。原插件保留原样；临时版本把原 `rate_limit(conf, ctx, plugin_name, 1)` 调用重复 k 次，
k 为 0、1、2、4、8，返回拒绝时立即退出。quota 为十亿/600秒，使用 local policy，正常请求不触发拒绝。
启动预检用连续请求的 `X-RateLimit-Remaining` 差验证真实调用次数，k=0 没有速率响应头。

18 个样本的顺序固定在 run.py 和每次 environment.json 中；Lua/C 顺序交替。
纯 NGINX、Lua 无插件、C 无插件都有前置/后置基线，其余组合各一个样本。
每个样本预热5秒，正式60秒；3,000计划请求/秒，32 keepalive连接，每样本180,000个请求。
gateway/upstream 分别绑定独立 CPU，负载器绑定另外3个 CPU。k=8 先做5秒容量检查，利用率低于70%才开始完整矩阵。

驱动复用 `../core_cost/load.py`，本次不启用其目录内任何探针。
记录实际发送至完整响应、计划到达至完整响应、客户端调度迟到三种分布；不丢弃迟到 slot，处理正常 keepalive 关闭。
每个窗口检查完整请求数、实际 backend hit 增量、HTTP错误、worker PID、CPU绑定和新增 error/alert 日志。
所有配置、临时插件、响应头、上游 echo、原始负载与结果保留。

## 分析

`analysis-plan.json` 固定公式和解释边界。它在矩阵进行中、看到早期基线之后写下，**不属于严格事前注册**。

1. 各基线取前后算术均值，同时报告端点与漂移。APISIX 均值减纯 NGINX 均值；枚举各端点检查敏感性，范围不是置信区间。
2. 原插件和循环 k=1 对比，检查注入循环/返回形状/JIT 的影响，不假定两者相同。
3. 各版本只对正 k=1、2、4、8 拟合 `CPU(k)=a+b*k`，报告残差、R²、留一预测误差。
   k=0 为独立空回调控制，因为它没有首次执行和响应头写入；不强行纳入同一直线。
   重复发生在同一个请求内，可复用该请求的上下文/配置缓存，不能视为 k 个独立真实请求。
4. b 是重复限流路径的增量，包含插件业务、核心 SDK、header 更新、GC 和 JIT/循环效应；a 是外推，不能直接命名为核心耗时。
   若假设插件业务 CPU 近似不变，Lua/C 斜率差可提示底层重复路径的节省；本工具不能独立证明该假设。
5. 分别比较实际 Latency；正常 GC 已在 CPU 中，不再叠加。每个正 k 只有一个样本，不输出统计置信区间。

本轮 k 顺序递增，与时间共变。高R²和较小残差不能排除漂移对斜率的影响；
若基线不稳，后续应使用紧邻交错基线与反转/随机化 k，而不是挑选更有利样本替换已有数据。

基线漂移5%、拟合最大相对残差5%、留一最大误差10%、k=1对原插件偏差5%、单核利用率70%为解释性诊断阈值，
不作为用户验收门槛，也不用于挑选更有利样本。若不近似线性就保留不线性的结论。

## 运行与复现

仅在专用 demo 容器执行，runner 会停止当前9080服务、建立临时配置，并在 finally 恢复原 limit-only native demo。
需要预先展开同一冻结源码、匹配的模块和参考 `results/environment.json`；
`/tmp/apisix-v6-limit-only-frozen/experimental/native-http-core/tools/demo.py` 使用归档的版本，
其中已明确关闭未启用的 Prometheus exporter。各次归档保存实际 helper 和 runner 哈希。

```sh
docker cp experimental/native-http-core/tools/subtraction/run.py apisix-native-demo:/tmp/subtraction-run.py
docker cp experimental/native-http-core/tools/core_cost/load.py apisix-native-demo:/tmp/subtraction-load.py
docker exec apisix-native-demo python3 /tmp/subtraction-run.py --verify --out /tmp/subtraction-verify --rate 3000
docker exec apisix-native-demo python3 /tmp/subtraction-run.py --out /tmp/subtraction-results --rate 3000
docker cp apisix-native-demo:/tmp/subtraction-results ./subtraction-results
python3 experimental/native-http-core/tools/subtraction/analyze.py ./subtraction-results
python3 experimental/native-http-core/tools/subtraction/verify.py ./subtraction-results
```

`verify.py` 不启动服务：核验算术/线性与非线性合成样本、拒绝不完整矩阵，再验证18份实际配置、
quota次数、上游header一致、原插件/模块/runner/driver哈希、CPU归一化、日志与恢复记录。

`--verify` 输出包含 k=8 的容量检查；正式运行独立输出，拒绝覆盖已有目录。
启动时核验固定模块 SHA 和冻结核心源文件哈希。上述路径与恢复 fixture 均针对这次历史 v6 研究环境，
迁移环境时应显式调整并记录路径，不应悄悄测成主分支后来版本。
旧 demo helper 的启动 banner 固定写 Prometheus :9091，不能作为是否启用的依据；以实际配置、插件检查和端口核验为准。
