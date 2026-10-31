# 2026-10-31：C key-auth 与原版 Lua Prometheus 混合执行

本轮回答：保留原版Lua Prometheus时，C路由、C认证、C代理是否仍可直接执行，以及相同功能
下相对稳定Lua的数据。上一轮[无指标的完整C专项](NATIVE_KEY_AUTH_V2.md)的结果继续保留，
不能直接拿它的+98.62%当作这次启用指标后的收益。

## 实现与比较口径

分支为`apisix-next-v4-llm-native-plugins-v3-hybrid`，基于`caf19a14`。原v1/v2分支及证据未改写。
当前稳定基线沿用APISIX 3.19.0，commit `473043623c161966f3f6ef4e8f12d2d8667a12d2`，
共同OpenResty 1.27.1.2/APISIX runtime 1.3.2和固定依赖（含API7 Prometheus库1.1.0，release rockspec为1.0.0）。这是源码在共同运行时上的比较，
尚未比较原装稳定发行镜像。所有场景均显式排除`ai`。

稳定版Prometheus的请求回调只有`log = exporter.http_log`。插件及其辅助Lua源码完全不修改。
新开关`apisix_core_lua_log_bridge on;`与`apisix_core_native_pipeline on;`配合：

1. C匹配路由、读取/隐藏凭据、认证、选择peer并经NGINX proxy发送响应。
2. 到NGINX log阶段才创建Lua api_ctx/var/matched视图，发布C已确定的路由、消费者、节点和响应来源。
3. 进入原有插件调度器执行未修改的Lua Prometheus，保留全局/路由优先级、metadata过滤、SDK getter/cache。
4. log结束后按原路径释放Lua变量和请求表，再释放配置代际；上游状态反馈只执行一次。

两种执行方式在同一NGINX worker内共存，Lua VM本来就用于初始化和配置timer。
本轮没有恢复Lua access/balancer/filter阶段，也没有增加第二层HTTP代理。
这次实现限定为key-auth加log-only Prometheus；需要其他Lua阶段的插件还需对应的桥接能力。
v2中的静态HTTP上游、只读配置对象等能力边界继续有效。

| 路径 | 核心/认证 | 指标 | 用途 |
| --- | --- | --- | --- |
| lua-both | 原版Lua核心、Lua key-auth | 全局原版Lua Prometheus | 用户要求的同功能基线 |
| c-adapter-both | C核心、C key-auth，保留原Lua阶段适配 | 同一Lua Prometheus | 请求协作方式对照 |
| c-hybrid | C路由/认证/peer，log阶段才进入Lua | 同一Lua Prometheus | 混合目标路径 |
| c-no-metrics | C路由/认证/peer | 不启用指标 | 衡量指标代价的参考 |

带指标的三条路径使用完全相同的业务资源：一条`/auth/*`路由、相同消费者/有效密钥、
hide_credentials、全局Prometheus，无其他插件；正式压力使用默认指标配置，无测试额外标签。
每样本5秒预热、60秒压力，单worker固定CPU0、上游CPU1、wrk t2/c32固定CPU2–4，真实代理3字节响应。
带指标路径均1Hz抓取；no-metrics参考不抓取。固定四路径正序、逆序各一轮，无吞吐选优。

## 正确性证据

[test_hybrid_prometheus.py](tests/test_hybrid_prometheus.py)对三条带指标路径使用同一批输入：

- 50个认证/代理场景，覆盖header/query/空密钥/重复值、凭据隐藏与匿名身份、请求体/HEAD、
  路由/方法/host、401/404/502/503/504、重试开关、有效超时和keepalive。
- status计数精确一致，路由、消费者、节点、匹配URI/host、response_source等标签逐series一致；
  route `prefer_name` 覆盖全局配置且恰好记一次，metadata条件过滤生效。
- histogram的request/upstream/apisix计数与+Inf桶相等；带宽与NGINX访问日志的实际
  request_length/bytes_sent精确相等。错误响应的chunked/content-length差异会改变线上字节，
  指标必须正确反映各自实际字节，并不要求不同编码方式的egress计数相同。
- 每路径128个并发认证，消费者计数分别64/64；客户端断连记录499；metric_errors_total为0。
- 在途请求跨密钥/路由名称更新仍使用原身份与路由标签；新请求使用新代际；Prometheus
  disabled_labels元数据缓存按版本失效；双worker的128个请求聚合准确。
- 通过自定义getter产生测试extra label并断言每请求仅读取一次，验证原SDK缓存。

原版Lua JSON watcher使用秒级mtime，C watcher使用CRC。最初快速测试更新写在同一秒内，
Lua基线未观察到文件变化；修正fixture更新时间后验证通过。另一个早期fixture把`$arg_skip`
误对应到query参数`arg_skip`，现改为`skip`。两次测试失败与原始指标均保留，未当作C性能结果。

模块ASan/UBSan、原v2四路径认证回归以及六配置核心回归（1967项检查）均通过；普通O3再验证混合语义。
运行时和其他DSO不带sanitizer，泄漏检查关闭，不能据此声称全进程无泄漏。

## 测量与诊断状态

原始证据写入`results/hybrid-prometheus-20261031/`，正式数据位于`final/round-*`。
[hybrid_experiment.py](tools/hybrid_experiment.py)提供validate/bench/diagnostics；
使用独立`APISIX_HYBRID_OUT`目录，已有label拒绝覆盖。

压力结束后等待异步导出收敛，验证status与request histogram计数一致、metric_errors为0。
计数包含预热及wrk停止边界的在途请求；与客户端完成数差异允许128（四倍并发）以内，
该检查在CPU测量窗口外进行。功能测试中没有停止边界干扰，逐请求计数/字节使用精确断言。

mean/P50/P95/P99/P99.9由同一wrk latency对象直接输出。两轮分位数均值并非合并请求后的
分位数。闭环驱动在各自吞吐下运行，不能替代相同固定RPS下的延迟/SLO曲线；两轮不证明统计显著性。

## 最终两轮60秒配对

[逐样本、汇总与校验](results/hybrid-prometheus-20261031/paired-summary.json)由[analyze_hybrid.py](tools/analyze_hybrid.py)直接读取原始wrk与环境文件生成。

| 路径 | 正序QPS | 逆序QPS | 平均QPS | CPU微秒/请求 | 峰值RSS MiB均值 |
| --- | ---: | ---: | ---: | ---: | ---: |
| lua-both | 16751.93 | 16784.14 | 16768.03 | 59.59 | 33.09 |
| c-adapter-both | 19845.35 | 19653.85 | 19749.60 | 50.57 | 20.22 |
| c-hybrid | 24408.23 | 24181.30 | 24294.76 | 41.10 | 19.87 |
| c-no-metrics | 47583.18 | 47663.94 | 47623.56 | 20.94 | 16.43 |

**同功能主要结论：c-hybrid相对lua-both平均QPS +44.89%，两轮分别+45.70%/+44.07%。**
相对保留旧适配器的C认证方案，混合闭环再提升23.01%（两轮+22.99%/+23.04%）。
相对不启用指标的C参考，加入Lua Prometheus后的吞吐下降48.99%；CPU/请求从20.94增至41.10微秒。
这个代价同时包含Lua指标业务、SDK视图/桥接、分配和导出工作，不能全部归因于Lua VM或单次跨语言调用。

8份正式样本均无响应/socket错误，测量期error.log为空，父级CPU节流为0。
三条带指标路径的资源/配置来源、依赖和编译一致；原版Lua未加载native模块。
压力后的request histogram与status总计相等，metric_errors为0；相对客户端完成数的边界增量为59–62，均在预定128以内。

### 延迟（毫秒）

| 路径 | 平均 | P95 | P99 | P99.9 正序 / 逆序 |
| --- | ---: | ---: | ---: | ---: |
| lua-both | 1.958 | 3.049 | 4.037 | 5.684 / 44.761 |
| c-adapter-both | 1.628 | 2.146 | 2.604 | 5.282 / 3.903 |
| c-hybrid | 1.320 | 1.782 | 2.183 | 3.981 / 3.453 |
| c-no-metrics | 0.711 | 0.844 | 1.094 | 35.268 / 2.641 |

混合模式相对原版Lua的平均/P95/P99均值分别下降32.60%/41.56%/45.92%。
P99.9保留逐轮原值：Lua基线第二轮44.761ms、无指标C第一轮35.268ms出现明显波动；
没有充分证据给这些极端样本归因。不能把两轮均值计算出的P99.9百分比当成稳定尾延迟收益。
均值和分位数来自同一wrk latency对象，与原始Thread Stats显示的mean一致（后者有显示舍入）。

## 分配与调用诊断

[诊断汇总](results/hybrid-prometheus-20261031/diagnostic-summary.json)来自独立15秒负载/10秒窗口，
不作为60秒吞吐样本。公开Lua allocator hook包含后台timer，计数不是物理malloc/RSS，也不涵盖NGINX pool。

| 路径 | VM新分配调用/接收请求 | VM申请字节/接收请求 | ac_push/请求 | ac_request/请求 |
| --- | ---: | ---: | ---: | ---: |
| c-adapter-both | 124.83 | 6936.08 | 94 | 13 |
| c-hybrid | 123.94 | 6892.46 | 约41 | 约5 |
| c-no-metrics | 0.000086 | 0.00467 | 本轮未插桩 | 本轮未插桩 |

混合模式仍每请求创建一次Lua视图，但只剩log阶段：start_phase/step各约1次，balance_start/
balance_finish和Lua适配器形式的key-auth回调均0。C认证直接执行约1次。旧适配器对应阶段入口
各2次及balancer入口各1次。调用插桩含开销，次数不能当作CPU百分比。

相比旧适配器，混合模式registry和阶段工作减少，VM分配量却接近，表明旧Lua Prometheus及其依赖的
工作仍然存在；不应把所有收益解释成GC消失。三条分配诊断均失败调用0、计数exact、hook恢复成功。

## 交付与仍不确定的问题

支持同一worker内C业务与Lua Prometheus混用，日志桥接开关默认关闭。配置资源按代际编译，
直接修改Lua配置对象不属于此只读配置模式的兼容承诺。其他Lua阶段/插件、更多路由与消费者、
不同硬件/并发及固定RPS延迟曲线仍需独立实现或测量。本轮没有扩展健康检查、服务发现或慢启动。

下一步优先在相同固定RPS下测延迟曲线，并对混合路径中仍存在的Prometheus分配采集Lua栈、
JIT trace exit及GC CPU证据，再决定是否改进指标框架。本轮调用/分配计数没有给出这些CPU占比，
也不能据此判断性能已到极限。

最终O3模块SHA-256为`6689e26a461d677fa4382489f30e389a9d472a55274ba932a4ce8464c0e65406`；同目录`final/module.so`、`module-source/`和`runtime.lua`
与正式样本匹配；诊断后已恢复相同哈希的普通O3，未留下测试网关。

[证据清单](results/hybrid-prometheus-20261031/MANIFEST.json)和[完整性检查](results/hybrid-prometheus-20261031/final-integrity.json)
包含所有正式、失败、诊断和控制日志；历史证据保持原哈希。工具/测试快照为交付版本，早期fixture修正记录
继续保留，不能把最终脚本当作每次失败运行的逐字脚本。
