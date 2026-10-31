# 原生 response 阶段实验：设计研究记录

本文件保留原型的设计依据，研究方式包括实际代码、编译和运行测试。
当前结果及限制见 [README.zh-CN.md](README.zh-CN.md)。
研究归档 PR 不以合并为目标，不代表产品功能交付；
Lua access 接管代理不作为本原生新阶段的验证结果。

## 目标与边界

保留 APISIX 的原生 `proxy_pass` 及其 upstream 建连、请求发送、响应解析路径。
在进入 response header filter chain **之前**新增独立、可 yield 的原生 response 阶段。
注入 Lua 可以读取完整的小文本响应、异步请求审核服务、修改待发送状态/头/正文；
事件就绪后恢复同一个请求，审核完成后才进入原有 header/body filter chain。

Demo 限于 HTTP/1.1、小文本、有限内存。正文上限 4 KiB，审核总时限 1.5 秒，
本地审核正常延迟 1 秒。超限、上游或审核失败都关闭未完成工作并返回固定错误。
不扩展为通用缓存、文件、SSE、HTTP/2/3、gRPC、子请求或任意 content handler 框架。
配置了审核但不受支持的响应必须明确报错，不能悄悄绕过审核。

## 已查明的源码依据

下载并检查 OpenResty 1.27.1.2，对应 Nginx 1.27.1、ngx_lua 0.10.28、
lua-resty-core 0.1.31。这与上一轮实际运行的 runtime 版本一致。
同时检查了对应 APISIX nginx module 1.19.2 的补丁入口。

源码保存在 `/workspace/native-response-phase-research/`，下载包校验：

```text
openresty-1.27.1.2.tar.gz
74f076f7e364b2a99a6c5f9bb531c27610c78985abe956b442b192a2295f7548

apisix-nginx-module-1.19.2.tar.gz
f95d621d80766d55bcea1ef7612411e776991fe6232427c7156b65e5cc6ea6e6
```

以下行号对应尚未叠加 APISIX 补丁的官方 bundle 源码：

| 源码 | 事实与设计影响 |
| --- | --- |
| `nginx/src/http/ngx_http_upstream.c:3026` | `ngx_http_upstream_send_response` 开始便调用 `ngx_http_send_header`；正文接收初始化在其后。必须拆开发送与接收。 |
| `nginx/src/http/ngx_http_core_module.c:1839` | `ngx_http_send_header` 直接进入 `ngx_http_top_header_filter`。新阶段必须在这之前运行。 |
| `nginx/src/http/ngx_http_upstream.c:3698` | 非缓冲接收循环会调用 `ngx_http_output_filter`，完成后 finalize；不能原封不动复用为“只接收不发送”。 |
| `nginx/src/http/ngx_http_upstream.c:4147` | 缓冲路径也会在 EOF/done 时 finalize。不能简单延后 header 而任其结束请求。 |
| `nginx/src/http/modules/ngx_http_proxy_module.c:2043` | 已有 framing 初始化及 content-length/chunked parser；应复用其解码结果，不另写 Lua 或 C HTTP 客户端去取业务上游。 |
| `nginx/src/http/ngx_http.c:454` | 普通 phase engine 在 content handler 返回 NGX_DONE 后已让出请求。响应阶段需要独立入口，不能只在 request phase 枚举里增加一项就认为能执行。 |
| `ngx_lua/src/ngx_http_lua_common.h:630` | `ctx->context` 是 uint16_t，现有 16 位已经全部占用。独立新阶段需要扩宽，同时检查 header/body filter 保存上下文的 uint16_t 临时变量。 |
| `ngx_lua/src/ngx_http_lua_sleep.c:129` | timer 回调依据 content 标志决定直接恢复还是重新跑 request phases。新 response 分支必须有自己的恢复路径。 |
| `ngx_lua/src/ngx_http_lua_socket_tcp.c:6134` | TCP 操作恢复后依据 content 标志处理 finalize。连接、读、写、超时等入口均须核对。 |
| `ngx_lua/src/ngx_http_lua_util.c:1682` | content 协程结束会自动发送结束链。不能把新阶段伪装成 content 来获得 cosocket。 |
| `lua-resty-core/lib/resty/core/phase.lua:33` | `ngx.get_phase()` 有独立的上下文映射；也要增加 `response`。 |

构建工具 checkout 的默认版本比上述基线更新，不直接执行默认 latest 构建。
实施时固定版本，先核对并叠加原 runtime 的 APISIX/相关模块补丁，再应用本实验补丁；
所有相关模块随核心重编译，不承诺二进制 ABI 兼容。

## 原型设计与补丁边界

### 1. Nginx core：独立的原生阶段

提供独立的 `NGX_HTTP_RESPONSE_PHASE` 注册表、运行游标和继续执行入口。
在 `ngx_http.c` 的 phase 初始化、core main conf 与调度逻辑中接入；
普通 request phase engine 不直接执行这一阶段，由 upstream 在完整正文就绪时进入。

处理器可以同步完成，或挂起当前阶段并让出 worker。挂起时保存阶段游标和 continuation，
事件回调恢复后继续当前处理器，再运行下一处理器，最后只提交一次响应。
不能把这个入口实现成一次不可挂起的 header filter 调用。

### 2. Native upstream：先受限收集，再提交

在 `ngx_http_upstream_send_response()` 调用 `ngx_http_send_header()` 之前增加显式开关分支。
未启用的请求保持原路径；启用的请求进入原生事件驱动收集状态机。

- 使用 proxy 模块现有 input filter/framing parser，处理已随响应头预读的正文和后续读取。
- 对解码后的字节逐次执行上限；复制到请求持有的有界缓冲后才能复用 upstream 原始 buffer。
- 收集期间不调用下行 header/body filter，不发送 flush/EOF，不写临时文件。
- 正文完整时进入新 response 阶段，不能走普通的完成即 finalize 分支。
- 在新阶段结束后确定最终 Content-Length，并进入原 header/body filter chain。
- 错误路径也只提交固定错误响应一次，不能把已缓冲的业务正文接在错误正文后面。

第一版优先实现 native proxy 的专用收集分支，避免为了实验重构所有 event_pipe 路径。
它仍由 Nginx upstream 接收业务响应，不由 Lua 重发请求。

### 3. ngx_lua：新上下文、注入与事件恢复

增加 `response_by_lua_block` / `response_by_lua_file`、配置合并与代码缓存入口，
并新增 `ngx_http_lua_responseby.c/.h` 的协程启动、挂起、恢复、结束逻辑。

- 增加独立 `NGX_HTTP_LUA_CONTEXT_RESPONSE`，扩宽 context 和保存/恢复它的变量。
- `ngx.get_phase()` 在这个阶段返回 `response`。
- 支持 TCP cosocket、`ngx.sleep` 和它们所需的 Lua 协程操作。
- 逐个接入 TCP 连接/读/写/超时、timer 回调到 response continuation。
- 正确处理 posted coroutine、异常与 NGX_AGAIN/NGX_DONE 的不同引用计数约定。
- 完成时返回 response 调度器；不重新进入 request/content，不发送 content EOF，不提前 finalize。
- 不通过全局放宽所有 yieldable API 来误开放尚未适配的 UDP、pipe、worker-thread 等入口。

这里的“事件回调”包括 Nginx I/O readiness、cosocket 超时、sleep timer 和客户端断开事件，
它们必须恢复或取消原 response 阶段。独立运行的 `ngx.timer.at` 回调属于另一个请求上下文，
不能拿它替代原请求的挂起/恢复机制。

### 4. Lua 对待发送响应的接口

拟新增实验 API：`ngx.response.get_body()` / `ngx.response.set_body()`，
并在新阶段支持 `ngx.status`、`ngx.header` 对未发送响应的修改。
读取和修改是对 native upstream 已收集的数据进行操作。

`ngx.print`、`ngx.flush`、`ngx.send_headers` 等直接输出 API 在此阶段不开放，
以保证 Lua 只能修改待提交响应。它们并非被改造成新的 body filter。

统一提交负责长度及结构化 header 字段一致性。拒绝时同时清除原上游实体/缓存/Cookie 等头、
旧 status line 与长度字段，设置 403 和固定正文，防止头列表与 Nginx typed fields 不一致。

### 5. 生命周期与失败收敛

收集、审核、输出各有明确的拥有者；等待时请求必须存活，完成/取消时释放等待引用。
上游连接可以在读取完成后释放，但不能顺带结束仍待审核的下游请求。

审核总时限由 C 层请求所属 timer 保证，不能只依赖 Lua 某一次 read 的 socket 超时。
超限、读取失败、Lua 异常、审核超时、客户端断开都进入统一的单次终止路径：
取消 timer/posted continuation，清理 Lua pending operation 和 cosocket，关闭未读完的 upstream，
释放正文状态。必须防止迟到回调访问已结束请求或再次提交响应。

### 6. APISIX 接入

在 `apisix/cli/ngx_tpl.lua` 的普通 `proxy_pass` location 中注入新指令，
在 APISIX Lua 中提供专门的 response phase 分发入口或实验脚本。
业务上游仍通过该 location 的原 `proxy_pass`，审核服务通过新阶段中的 Lua cosocket 请求。
不使用 access 接管插件。

## 拟验证的完整时序

```text
原生 proxy_pass
  -> native upstream 解析响应头
  -> COLLECTING：头不下发，正文受限收集
  -> RESPONSE：执行注入 Lua
       -> 审核 cosocket 发起 I/O，Lua yield
       -> worker 返回事件循环，普通请求照常完成
       -> I/O / timer 事件回调，resume 同一 response 阶段
  -> COMMIT：确定最终 status / headers / body
  -> ngx_http_send_header
  -> 原 header filter chain
  -> 原 body filter chain
  -> FINALIZE

任意等待状态 -> 错误或取消 -> 单次清理
  存活客户端：固定错误响应进入原 chain
  已断开客户端：终止并释放资源
```

为证明这真是新阶段，除黑盒测试之外还必须加入 C 层阶段日志/计数：

1. `body_complete < response_enter < yield < resume < response_leave < header_chain_enter`。
2. Lua 的 `ngx.get_phase()` 返回 `response`；header filter 仍是原 `header_filter`。
3. 审核未完成时，header chain 进入次数为 0、`r->header_sent` 为 0，下行无任何字节。
4. 拒绝时 header filter 第一次运行就看到 403；native `$upstream_status` 仍反映业务上游状态。
5. 单 worker，审核延迟 1 秒时另一条普通 proxy 请求能在审核完成前返回。
6. 覆盖通过、拒绝、chunked、恰好上限、两类超限、截断、审核失败、总时限与滴流。
7. 覆盖收集和审核两个阶段的客户端断开，检查 timer/socket/引用计数、迟到回调和重复提交。
8. 未启用新阶段的普通路由作为回归对照。

研究交付物包括可复现的源码补丁、隔离构建/启动命令、实测记录和指导说明。
Draft PR 仅保存这些研究材料，不扩展为生产开发或合并交付。

## 替代路径与结论状态

Lua access 接管代理、content 子请求捕获、外置审核代理都属于可减少核心改动的替代路径，
但不能验证用户要求的原生新阶段，因此不作为本轮实现。
放宽 header/body filter 的 cosocket 检查也不能补齐 upstream 收集、恢复和请求生命周期。

这些入口已经通过原型编译、运行和干净补丁重放验证；结论限定在 README 中列出的
小文本原生 proxy 场景。其他响应来源、协议和生产级生命周期保证仍需后续研究。
