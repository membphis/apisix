# 原生 response 阶段的可行性研究结果

**研究归档，不以合并为目标。** 本文与随附示例保存一次技术预研的背景、目标、
方案、原型代码和实测证据。它不是 APISIX 产品功能提案，也不表示已达到生产代码标准。
Nginx/ngx_lua C 代码需要单独编译进定制 OpenResty，不能在普通 APISIX 中直接使用。

## 背景与研究问题

响应审核需要看到完整正文，再决定客户端最终收到的状态码、响应头和正文。
例如，上游返回 201 和一段文本，审核通过则保留原响应；审核拒绝则返回 403 和固定提示。
审核服务需要网络调用，且可能延迟 1 秒。在等待期间不能先把响应头或待审核内容发给客户端，
也不能阻塞同一个 Nginx worker 的其他请求。

现有 header/body filter 不提供这样的可等待网络阶段。实际探测中，两种 filter 的 cosocket
都被禁止。仅在 body filter 中缓存正文、打开 proxy_buffering，或启动独立 Lua timer，
不能证明完整正文审核后才进入 header chain 的时序。
Lua access/content 接管上游可以验证另一种代理方案，但不能回答本研究的问题：
**能否保留原生 proxy_pass，在 response header chain 之前新增独立、支持 Lua 异步恢复的阶段？**

## 目标及成功标准

本次只验证小文本原型，避免把研究扩大为生产框架：

- 业务上游仍由原生 Nginx upstream 发起请求、解析响应，Lua 只调用本地模拟审核服务。
- 受限收集完整正文，随后进入独立 `NGX_HTTP_RESPONSE_PHASE`，注入的 Lua 能 yield/resume。
- Lua 使用 TCP cosocket 或 sleep timer 后，事件回调继续同一个 response 阶段。
- 审核结果确定后，才首次进入原 header chain，并统一提交状态、响应头和正文。
- 原型使用 4 KiB 正文上限、1.5 秒阶段总时限，覆盖失败关闭、断开取消和资源回收。
- 结论必须有编译产物、真实 HTTP 测试、C 层时序证据和干净源码重放支持。

## 研究方法与原型

首先检查 APISIX 模板、原生 upstream、ngx_lua 调度和上下文定义。
关键发现是 `ngx_http_upstream_send_response()` 开始就调用 `ngx_http_send_header()`，
正文接收初始化反而在后面；因此必须拆开发送与接收。另一个约束是 ngx_lua 的 16 位
上下文标志已满，而且 content 协程完成会自动处理 EOF/finalize，不能把新阶段伪装成 content。

原型随后修改并重新编译了 Nginx core/upstream、ngx_lua、lua-resty-core 共 25 个文件：

```text
native proxy_pass
  -> 原生响应解析与有界正文收集
  -> 独立 response 阶段
       -> Lua cosocket / timer yield
       -> worker 处理其他请求
       -> 事件回调 resume 同一 response 阶段
  -> 确定最终 status / headers / body
  -> ngx_http_send_header
  -> 原 header filter chain / body filter chain
  -> finalize
```

新增上下文及专用 continuation，扩宽上下文存储和保存变量，适配 TCP/sleep 的事件恢复。
Lua 可读取/修改待发送正文、状态和头，但 `ngx.print/flush/send_headers` 仍被禁止。
原生 C 计时器控制阶段总时限，取消路径清理 Lua pending operation、cosocket 和正文状态。
APISIX 通过现有模板 snippet 加载实验指令，正常路由和原 `proxy_pass` 保留。

这份代码是为研究假设服务的技术原型，运行时构建与正常 APISIX 产品构建分开。
详细设计见[设计记录](../../example/native-response-phase/PROPOSAL.zh-CN.md)，
实际配置与复现命令见[实验说明](../../example/native-response-phase/README.zh-CN.md)。

## 实测结果与证据

2026-10-31 UTC，Linux x86_64。APISIX 源码基线为
`b31e828c6f6b588ae954bf69c8432c08fb73bb7e`；OpenResty bundle 1.27.1.2，
Nginx 1.27.1、ngx_lua 0.10.28、lua-resty-core 0.1.31。HTTP worker 数固定为 1。

| 验证项 | 结果 | 可核对材料 |
| --- | --- | --- |
| 黑盒 HTTP 检查 | 主实验和干净补丁重放各 33 项通过 | [主实验](../../example/native-response-phase/results.json)、[重放](../../example/native-response-phase/replay-results.json) |
| 并发 | 审核 1.0023 秒；普通请求 0.0018 秒，且在审核仍 active 时完成 | 同上并发检查项 |
| 提前发送 | 审核开始后裸 socket 连续 300 ms 没有收到任何字节，包括状态行和头 | 同上静默窗口断言 |
| 新阶段身份与时序 | `ngx.get_phase()` 返回 response；所有 yield/resume 发生在首次 header chain 之前 | [C trace](../../example/native-response-phase/trace-results.json)、[摘录](../../example/native-response-phase/trace-excerpts.txt) |
| 状态与正文统一决定 | 原生 upstream_status 仍为 201，后续 header filter 第一次便观察到 403；固定拒绝正文 27 字节 | trace 摘录、HTTP 结果 |
| 两种注入入口 | block/file 均运行；timer 恢复、状态和正文修改通过 | HTTP 结果中的 inline probe、sleep、审核用例 |
| 边界与失败 | 上限、chunked、UTF-8、截断、审核失败、Lua 异常、总时限和持续滴流通过 | 主实验与重放结果 |
| 断开与资源 | 额外 20 次断开后 FD 为 18 → 18；两类活动请求归零、worker PID 不变 | [资源记录](../../example/native-response-phase/resource-results.json) |
| 构建重放 | 25 个源码文件及重编译二进制均逐字节一致 | [清单](../../example/native-response-phase/evidence-manifest.json)、[运行时证据](../../example/native-response-phase/runtime-evidence.txt) |

主实验 C trace 记录 58 个 native 请求、71 次 yield、54 次 resume；每个请求至多提交一次，
30 个请求在发送头前取消并清理。58 是包括重复断开在内的 native 请求数，不是测试项数。
等待期间请求引用计数为 1，`header_sent=0`、`header_calls=0`。
日志检查允许主动触发的 Lua 异常和提前关闭上游夹具，拒绝其他 error、alert/crit/emerg、
重复发头和 worker 信号退出。资源实验只证明有限次数回收，不是长期泄漏或压力测试结论。

原型二进制 SHA256：
`1464f5ce70a668517668b1c5401490b0cdaa37745c3f830057f73383a9dc4b4f`。

## 代码与复现入口

| 材料 | 用途 |
| --- | --- |
| [核心 diff](../../example/native-response-phase/native-response.patch) | 展示全部 25 个 native 文件的改动，在固定 API7-patched bundle 上重放 |
| [完整 C/H 示例](../../example/native-response-phase/source-examples/) | 从实测工作树冻结的新增源文件，可直接阅读，不是 APISIX 可编译模块 |
| [adapter 输入](../../example/native-response-phase/core/) / [生成器](../../example/native-response-phase/adapt.py) | 继续预研时生成相同源码；完整 Lua loader 已包含在冻结源文件及 diff 中 |
| [源码准备](../../example/native-response-phase/prepare.py) / [依赖准备](../../example/native-response-phase/prepare-deps.py) | 固定版本下载、SHA256 验证、原 runtime 补丁和原型补丁应用 |
| [构建脚本](../../example/native-response-phase/build-nginx.sh) / [compiler 镜像](../../example/native-response-phase/Dockerfile.toolchain) / [runtime 镜像](../../example/native-response-phase/Dockerfile.runtime) | 隔离重编译和运行 |
| [Lua 审核](../../example/native-response-phase/audit.lua)、[配置](../../example/native-response-phase/config.yaml) | 新阶段注入及 1 秒模拟审核 |
| [HTTP 检查](../../example/native-response-phase/verify.py)、[时序检查](../../example/native-response-phase/verify-trace.py)、[资源检查](../../example/native-response-phase/verify-resources.py) | 复现成功标准 |
| [原始实验记录](../../example/native-response-phase/evidence-logs/) | 主实验、重放、编译和补丁准备的文本记录，可直接复查 C trace |

完整启动命令在实验说明中。先准备一个新的工作目录，重放固定源码和补丁，
在容器中编译定制 Nginx，再运行 APISIX 实验镜像及上述检查。
仓库不提交下载依赖、编译产物、运行时私有文件或旧的 access 接管 Demo。
已有运行结果对应记录的源码和二进制，PR 归档本身不会使 stock APISIX 获得新阶段。

## 结论、限制及采用决策

**已确认：在所测原生 HTTP proxy、小文本、单 worker 场景中，新增可异步执行 Lua 的
response 阶段，并在 header chain 前统一决定响应，是可行的。**

以下仍是后续设计和研究问题，不属于本原型已经证明的能力：

- 缓存命中、store、X-Accel/internal redirect、error_page、子请求和其他响应来源的统一接入。
- HTTP/2/3、gRPC、升级协议、压缩、流式响应、大正文、磁盘缓冲及下行背压。
- 更多异步 API、自定义 Lua on_abort、插件顺序、实体头规则、重试、统计和 keepalive。
- 高并发、长期内存、OOM、reload/worker 退出、跨版本和二进制 ABI 兼容。

阶段总时限只约束事件循环可以调度的异步等待，不保证抢占无限 Lua 计算。
本次没有做生产框架、APISIX 全量回归或兼容性认证，也不以默认 CI 或合并就绪作为成功标准。
是否采用该路径，以及采用后的高质量产品设计和实现，需要后续独立决策。
