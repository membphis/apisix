# 2026-10-31：key-auth 原生业务与 C 请求闭环专项

用户确认此处的“Chaos”指 `key-auth`。本轮允许把插件业务改写为 C；它与
[插件不改时的上一轮研究](EXHAUSTIVE_20261031.md)是不同的比较边界。

## 阶段记录与实验设计

上一阶段代码及正负测量已提交：`634b6b6093751041e85b0bf5b8b27e752a9a8682`；
完整归档检查点为`b839232031a3797a1ddcee9b20f8a81f4ef0bf72`，含原始日志及输出空白属性，
历史证据字节未改变。原分支及
[`apisix-next-v4-llm-core-v1`](https://github.com/membphis/apisix/tree/apisix-next-v4-llm-core-v1)
均已推送。本专项在 `apisix-next-v4-llm-native-plugins-v2` 继续。

四条路径使用相同 APISIX 3.19.0 插件/配置源码和共同固定的历史运行时，业务路由只启用
key-auth，AI 及 Prometheus 不加载。所有 Native 路径均使用 O3 和上一轮选中的六项设置。

| 路径 | 认证逻辑 | 请求阶段与上下文 | 上游衔接 |
| --- | --- | --- | --- |
| lua-core | 稳定版原 Lua key-auth | 稳定版 Lua 核心 | 原版 Lua balancer |
| native-lua（方案一） | 同一份 Lua key-auth | 既有 C 核心，保留 Lua ABI/协程适配 | 既有 Lua balancer 帧调用 C |
| native-c-adapter | 新 C key-auth | 保留方案一的 Lua 阶段帧和上下文表 | 同方案一 |
| native-c-pipeline（方案二） | 同一份新 C key-auth | C access handler、C 路由及原生请求状态 | C peer callbacks + NGINX 原 keepalive |

中间路径用于分开评估业务移植与请求协作方式改变；这些差值不能脱离本场景归因为某个
跨语言调用的固定成本。预先固定四路径正序、逆序各一轮，每样本5秒预热、60秒压力。
有效 header 密钥、隐藏凭据、真实代理、3字节响应、wrk t2/c32/keepalive。网关一个 worker
固定 CPU0，上游 CPU1，驱动 CPU2–4；父级4核配额。每份原始证据包含配置、构建/源码哈希、
CPU、RSS、延迟及节流统计。共同依赖并非原装稳定版发行镜像，硬件和统计边界沿用上一轮。

## 实际实现

- [key_auth.c](module/key_auth.c)：配置代际内编译大小写敏感的消费者密钥索引和认证配置。
  C 实现 header 优先、query 回退、重复值处理、空密钥、匿名消费者、WWW-Authenticate、
  隐藏凭据、消费者身份头以及日志。使用既有 schema 校验/defaults；共享函数供两条 C 业务路径执行。
- [pipeline.c](module/pipeline.c)：C access/log handler 持有配置代际，直接连接 C 路由、
  C key-auth 和代理准备；请求不创建 Lua api_ctx/var/matched 表，不往返 Lua engine。
  旧配置的 Lua references 在配置 watcher 中回收，在途 C 请求只增减代际引用数。
- [routes.c:ac_match_native](module/routes.c)：复用原 C 压缩前缀树、host/method/CIDR/path 判定和
  既有 precedence。没有更换路由算法；去掉当前 C 插件用不到的 Lua ABI 发布和候选数组排序。
- [upstream.c](module/upstream.c)：C `init_peer/get/free` 复用既有节点选择/状态反馈，直接连接
  NGINX proxy。先核查并复用 NGINX 自带 keepalive wrapper，没有复制一套连接池实现。
  同时保留 retry-off/失败类型限制、有效超时和节点优先级。
- [http.c](module/http.c)、[request.c](module/request.c)、[native_phases.c](module/native_phases.c)：
  C 转发变量/请求头准备、原生响应头及 filters；没有 Lua request 阶段。
- [tools/demo.py](tools/demo.py)：方案二配置移除业务 access/filter/log/balancer Lua hooks，
  将 `apisix_core_native_balancer;` 放在 stock keepalive 前。配置与后台 timer 仍使用现有 Lua 初始化。

新开关默认关闭：`apisix_core_native_key_auth on;` 启用 C 业务加旧适配器；
`apisix_core_native_pipeline on;` 启用完整 C 请求路径（隐含前者）。这是显式实验模式，
会在事务编译时拒绝不支持的配置，不会静默略过其业务。关闭开关可回到方案一。

## 语义范围与边界

目标是 standalone、route-only key-auth、literal keys、单个静态 HTTP 上游的请求闭环。
覆盖 header/query、消费者/匿名身份、隐藏凭据、透传请求体/HEAD、常用路由判定、重试、
超时、keepalive 和热更新。此模式只允许 key-auth 插件列表；不执行其他 Lua 插件或多认证编排。
Lua route/filter/metadata callbacks、metadata error_response、secret provider、消费者/组引入
其他插件均会拒绝。完整 C 路径暂不支持 tracing、servlet/encoded-slash 扩展、trusted-address
扩展、DNS/chash/HTTPS、pass_host=node 或非共同配置的 keepalive 参数。

因此它不是全部 key-auth 部署方式的通用替代品，也不把这些限制写成原 Lua 插件没有的要求。
配置/校验/监控端点仍依赖 LuaJIT；“纯 C”指业务请求热路径没有 Lua VM 执行，进程并未移除
ngx_lua/LuaJIT。请求头复用既有导出的 ngx_lua **C** 接口，不经过 Lua 或 FFI 调度。

NGINX stock keepalive 缓存按同一 upstream group 的地址复用，而旧 Lua 动态池按
scheme/IP/port 分池；本场景只有同一 HTTP endpoint，320/1000/60s 参数一致，不能推广为
多 endpoint 超出缓存容量时的完全等价。stock age 上限设为64位有符号毫秒最大值，避免默认
1小时连接年龄限制；它是类型上限，未做长期老化试验。URI 点路径转发继续沿用本研究分支
已披露的策略，与稳定 Lua 有差异；本轮未把该差异当作新增收益。

## 验证、测量与原始证据

证据目录：[results/native-key-auth-v2-20261031/](results/native-key-auth-v2-20261031/)。
失败和修复记录保留，包括：最初覆盖 retry-off 的错误，以及只更新 conf 而未更新 runtime
提前缓存的有效超时字段的问题。两者都由扩展差分发现，修正后才接受正式吞吐测量。
补查 HEAD 时又将拒绝分支改为显式 finalize/NGX_DONE；旧候选的实际复现为401、上游命中0次，
不能把源码中的后续阶段风险写成已发生的越权转发。已完成的3个60秒修正前样本保留在根目录，
最终候选的验证与8个配对样本另存`final/`，不混用两批二进制。

重现工具：[tools/key_auth_experiment.py](tools/key_auth_experiment.py)。在准备好的容器内用
新的 `APISIX_KEY_AUTH_OUT` 依次运行 `validate`、`bench`、`diagnostics`；已有 label 拒绝覆盖。
业务验证见 [tests/test_native_key_auth.py](tests/test_native_key_auth.py)。

四路径各47个语义场景均通过（O3与模块ASan/UBSan）：header/query优先级、重复值、空值、
特殊字符、超过100个query字段、header数量上限、自定义realm、匿名消费者及凭据双位置清理、
消费者头去伪造、禁用认证、路由边界、方法/Host、请求体/HEAD、50ms/3s有效超时、重试开/关。
每路径另有128个并发认证请求、keepalive连接ID以及GET/HEAD/POST拒绝不抵达后端检查。
两条C业务路径另通过在途请求持有旧代际、密钥轮换、拒绝不支持的热更新和随后恢复。
默认关闭新功能时，六配置ASan核心回归合计1,967项通过；最后的HEAD结束分支只影响完整C路径，
修正后重跑了相关四路径验证，没有重复六配置回归。运行时及其他DSO未加sanitizer，泄漏检查关闭，
因此不是全进程内存安全或无泄漏证明。

### 最终60秒配对

原始证据：[正序Lua](results/native-key-auth-v2-20261031/final/round-1-lua-core/results.json)、
[正序方案一](results/native-key-auth-v2-20261031/final/round-1-native-lua/results.json)、
[正序C适配器](results/native-key-auth-v2-20261031/final/round-1-native-c-adapter/results.json)、
[正序完整C](results/native-key-auth-v2-20261031/final/round-1-native-c-pipeline/results.json)；
逆序同目录的`round-2-*`保留完整配置与原始wrk。
[汇总与逐样本数据](results/native-key-auth-v2-20261031/paired-summary.json)由
[analyze_key_auth.py](tools/analyze_key_auth.py)直接核对原始QPS、请求数、错误日志、资源和构建哈希生成。

| 路径 | 正序QPS | 逆序QPS | 平均QPS | CPU微秒/请求（均值） | 峰值RSS MiB（均值） |
| --- | ---: | ---: | ---: | ---: | ---: |
| lua-core | 23605.56 | 23458.46 | 23532.01 | 42.46 | 30.64 |
| native-lua（方案一） | 25997.77 | 25214.73 | 25606.25 | 39.01 | 17.29 |
| native-c-adapter | 28409.06 | 28794.92 | 28601.99 | 34.91 | 17.22 |
| native-c-pipeline（方案二） | 46669.37 | 46808.48 | 46738.93 | 21.32 | 16.49 |

- 仅C认证业务及其数据结构、保留旧适配器：相对方案一均值 **+11.70%**（两轮+9.27%/+14.20%）。
- 同一份C认证逻辑，改为完整C请求协作：相对C适配器均值 **+63.41%**（+64.28%/+62.56%）。
- 用户两方案直接比较：方案二相对方案一均值 **+82.53%**（+79.51%/+85.64%），CPU/请求约下降45.34%。
- 方案二相对稳定Lua均值 **+98.62%**（+97.70%/+99.54%）。本轮方案一相对Lua为+8.81%，
  是本轮重新配对的结果，不替换上一轮不同时间的+3.97%。

8份正式样本均60秒，响应/socket错误为0，采样期error.log为空，CPU节流为0。
配置、业务资源、运行时和编译选项一致；Lua对照不加载native模块。没有按吞吐结果改选参数。
每条路径的p99两次分别为：Lua 2.063/2.168ms、方案一1.898/1.953ms、C适配器1.683/1.655ms、
完整C 1.040/1.067ms。这是闭环驱动在各自吞吐下的延迟，不是相同外部到达率下的SLO证明。
两轮也不证明统计显著性、硬件通用收益或绝对上限；本专项结果不能推广到Prometheus/limit-count。

最终普通O3模块SHA-256为`99bb4233fa8780c7ace4395f8ff52ead0398a77b31218face9713031dbbdab5f`；
[最终二进制](results/native-key-auth-v2-20261031/final/module.so)与同目录`module-source/`、
`runtime.lua`、`final-o3-build.json`一同保存。诊断结束已恢复完全相同哈希的普通O3构建。

正式测量后，收尾检查修正了配置生成器中的代理标记作用域：原先server级的
`apisix_core_proxy on`会让独立`nginx_status`入口被C路由拦截为404。标记改放业务location后，
四路径均验证该入口200，并再次通过47个业务场景、并发与热更新检查。
[精确前后配置核对](results/native-key-auth-v2-20261031/postcheck/config-scope-correction.json)
证明生成配置仅移动这一处声明；业务location有效值均为on，资源与C二进制相同。原始8份实测
配置仍保留，没有改写，也没有把这次辅助入口修正说成新测得的性能收益。

### 为什么增益主要在请求协作

[分配与函数诊断汇总](results/native-key-auth-v2-20261031/diagnostic-summary.json)对应独立的
15秒诊断负载、10秒窗口；不计入60秒正式吞吐样本。使用既有公开Lua allocator hook，包含后台timer；
函数插桩有开销，计数不能当作CPU占比。结果为：

| 路径 | VM新分配调用/接收请求 | VM新申请字节/接收请求 | ac_push/请求 | ac_request/请求 |
| --- | ---: | ---: | ---: | ---: |
| 方案一 | 15.0093 | 1274.09 | 约71 | 约10 |
| C适配器 | 7.0077 | 441.97 | 约74 | 约11 |
| 完整C | 0.0000866 | 0.00472 | 0 | 0 |

完整C的485,061次接收请求窗口中共42次VM新分配调用、2,288申请字节；这些总量包含后台timer，
不等于请求直接分配，也不表示没有NGINX pool、libc或其他C分配。三条路径分配失败均0，hook均正确恢复。
函数诊断中完整C有367,801次`ac_http_prepare_native`/`ac_key_execute`，`ac_phase_start`、
`ac_phase_step`、`ac_balance_start`、`ac_balance_finish`、`ac_request`和`ac_push`均0。
这与生成配置没有业务Lua阶段、typed request无需Lua表/registry的源码路径一致。

旧formatter只以`ac_http_prepare`归一化，所以完整C的原始派生JSON曾显示`requests:0`/比例null；
原始计数保留，汇总改用`ac_http_prepare_native`，并修正后续formatter且用真实原始dump验证。
不能将这个格式问题写成“没有请求”。C适配器的registry helper次数反而较高却仍更快，
说明次数不能直接当作成本；它减少了Lua业务/SDK转换和分配，仍保留原来的阶段帧与上下文发布。

完整C的额外收益同时包含协程/阶段入口、Lua ABI/registry/表、变量发布、路由候选准备、响应头和
balancer/pool衔接变化；不是只删除一个Lua函数调用。两个增益比例不能相加，也不能全部归给GC。

## 结论与下一步

在允许重写key-auth、明确限制配置能力的本专项中，完整C链路超过了30%–40%的目标。
上一轮“插件不改”的结论不变；这里没有证明任意Lua插件都能获得同等收益，或通用框架已经达到上限。
更大的收益来自C业务与C请求状态/阶段/代理直接对接，单独把Lua业务翻译成C仍会留下大量旧接口工作。

下一步优先把实验中的C插件编译/执行接口整理为可复用接口，再测更多路由/消费者、并发和硬件。
若扩展到其他插件或配置能力，须先实现对应语义和差分验证；尤其是指标、共享限流、动态getter、
多endpoint池容量、secret provider等不能套用本次key-auth结果。当前代码作为实验分支保留，不默认开启。

[归档清单](results/native-key-auth-v2-20261031/MANIFEST.json)列出源码/二进制、正式和修正前样本、
验证与诊断文件的字节数和SHA-256；清单不含自身。
[完整性检查](results/native-key-auth-v2-20261031/final-integrity.json)验证本轮11个60秒样本、
最终源码/构建，以及历史229、上一轮182和扩展研究698份文件未改变。
`harness/`是交付时的工具和测试快照；早期失败并非每一次都有独立二进制/脚本快照，具体修复
和控制进程暂停记录保留在日志中，不把最终脚本冒充历史每次运行的原脚本。
