# 原生 response 阶段：技术预研与可运行原型

**研究归档；不以合并为目标，不是可直接在现有 APISIX 上启用的产品功能。**
本目录随 Draft PR 保存方案、原型及证据，供后续采用决策参考。
背景、研究问题与证据索引见[研究报告](../../docs/research/native-response-phase.zh-CN.md)。

## 预研结论

**在本实验限定的原生 HTTP `proxy_pass` 场景中，技术路径已跑通。**
实际修改并重编译了 Nginx core/upstream、ngx_lua 和 lua-resty-core，新增独立的
`NGX_HTTP_RESPONSE_PHASE` 与 Lua `response` 上下文。业务响应由原生 upstream 接收，
完整、受限地收集正文后运行注入的 Lua；Lua cosocket/定时器挂起与恢复完成后，
才第一次进入原 header filter chain。

这是技术原型，不是生产框架。目标是给是否采用这条路径提供代码与实测依据，
不承诺所有响应来源、协议、模块组合或异常资源条件都已解决。
Lua access 接管代理或子请求捕获不在本次验证路径中。

## 实际改了什么

核心补丁 [native-response.patch](native-response.patch) 修改/新增 25 个源码文件。
它基于 OpenResty 1.27.1.2 bundle，先叠加固定版本 API7/multi-upstream 补丁，再应用本补丁。
构建的是供本实验使用的 Nginx 二进制及所需模块子集，不是完整 APISIX runtime 发行包。

| 层次 | 本次代码 |
| --- | --- |
| Nginx core | 增加 `NGX_HTTP_RESPONSE_PHASE` 注册表，普通 request phase engine 跳过它；由独立 response continuation 调度。 |
| Native upstream | 在 `ngx_http_upstream_send_response()` 发送响应头之前分支，复用 proxy input filter/framing parser，受限收集正文，保留原 `proxy_pass` 请求。 |
| 响应提交 | 单一提交入口设置最终 Content-Length，再调用 `ngx_http_send_header()` 和原 body filter；避免原 upstream finalizer 再发送一次 EOF。 |
| ngx_lua | 新增 `response_by_lua_block/file`、独立协程入口与恢复函数；扩大已用满的上下文标志和保存变量至 32 位。 |
| I/O 与 timer | TCP connect/read/write、socket timeout 和 sleep timer 恢复到 response continuation；不重入 access/content，不借用 content 标志。 |
| Lua API | 新增 `ngx.response.get_body/set_body/clear_headers`，允许修改待提交的 `ngx.status`、`ngx.header`；直接输出 API 保持禁用。 |
| APISIX | 通过模板已有的 location snippet 注入新指令和实验脚本；正常路由、balancer 和 `proxy_pass` 继续工作，没有 access 接管插件。 |

`core/` 是 adapter 的输入；[source-examples/](source-examples/) 保存从实测工作树
提取的完整新增 C/H 文件，便于直接浏览，包括生成后的 Lua code-cache loaders。
`adapt.py` 用断言检查源码锚点后生成修改，`native-response.patch` 是可重放的完整 diff。
这些 C 文件属于定制 OpenResty 构建，不能直接复制到 APISIX Lua 插件目录使用。

```text
原生 proxy_pass -> upstream 响应头解析 -> COLLECTING（有界正文）
  -> 原生 RESPONSE 阶段 -> 注入 Lua -> cosocket / sleep yield
  -> 事件循环继续处理其他请求 -> I/O / timer callback -> resume
  -> 决定 status / headers / body -> COMMIT
  -> ngx_http_send_header -> 原 header chain -> 原 body chain -> finalize
```

收集和等待期间没有进入下行 filter，也没有提前发送 flush/EOF。
关闭原业务 upstream 连接与结束客户端请求分开处理；请求在异步等待期间保持存活。
超时、Lua 异常和客户端断开都会取消请求所属 timer/pending operation，清理 cosocket。

## 演示范围和接口

`/native/*` 仍由 APISIX 正常路由到 `127.0.0.1:1980` 的本地上游。
`/demo/audit.lua` 在新阶段把已经收集的正文 POST 到 `127.0.0.1:1981/review`。
两项服务由 `mock.conf` / `mock.lua` 提供，和 APISIX 共用一个 HTTP worker。
正常审核通过与拒绝均使用 `ngx.sleep(1)` 模拟 1 秒延迟。

| 边界/结果 | 本原型行为 |
| --- | --- |
| 上游正文 | 最多 4096 字节，未压缩 `text/plain`；支持所测 Content-Length 和 chunked 响应。 |
| 通过 | 原状态、正文原字节及原生 proxy 的响应头处理继续生效；保留多值 Set-Cookie。 |
| 拒绝 | 403，固定 `response rejected by audit\n`；清除原上游响应头。 |
| 正文超限 | 502，固定 `upstream body too large\n`，不调用审核。 |
| 正文收集总时限 | 1200 ms，从上游响应头解析完成开始计算，超时返回 504。建连/响应头仍由原生 proxy 超时控制。 |
| 新阶段总时限 | C 层请求所属 timer，1500 ms；覆盖等待审核和持续滴流，超时返回 504。 |
| 审核回复 | 最多 64 字节，必须为 200 且正文严格等于 `allow\n` 或 `deny\n`，否则 502。 |
| Lua 抛异常 | 固定 500，终止该阶段，不发送原业务正文。 |
| 不支持的类型/压缩响应 | 在已接入的路径中返回 502。 |
| 上游尚未返回响应头就断连 | 沿用 Nginx 原生 502 错误响应，不伪称已经进入新阶段。 |

所有所测最终响应都检查了 Content-Length 与实际字节数一致，包括 UTF-8、空 201、
恰好上限及固定错误正文。原 chunked 传输经过 native parser 解码后重新确定长度。
“原样”不要求报文逐字节相同：Server 等仍由原有网关/filter 控制。

`/plain` 是未启用新阶段的普通原生 proxy 对照。
`/block` 额外验证 inline Lua 注入、50 ms sleep 恢复、修改 202/正文，
以及后续 header filter 观察到最终状态；主审核路径使用 file 注入。
`/stats`、`/capabilities` 是仅用于本地实验的观测端点。

实验指令：

```nginx
response_phase $native_response_enabled;
response_body_limit 4k;
response_collect_timeout 1200ms;
response_timeout 1500ms;
response_by_lua_file /demo/audit.lua;
```

Lua 中 `ngx.get_phase()` 实测返回 `response`，包括异步恢复后。
`ngx.print/flush/send_headers` 在这里仍被禁止，Lua 只能修改尚未提交的响应。
现有 `header_filter/body_filter` 中的 cosocket 依然禁用，已有实际探测。

## 从材料重放并启动

需要 Linux、本地 Docker Engine、Python 3.12+、Git 和 patch。
Docker 存储应预留足够空间；使用 vfs 时建议约 20 GB。编译在容器中进行，
宿主无需安装 Nginx/OpenResty/Lua 开发库。

APISIX 源码基线为 `membphis/apisix@b31e828c6f6b588ae954bf69c8432c08fb73bb7e`。
本 PR 的实验目录应保持在 checkout 的 `example/native-response-phase/` 下；
它只保存研究材料，不会自动改变 APISIX 的构建或运行入口。
以下从 APISIX 仓库根目录执行，工作目录必须是新的：

```bash
export NATIVE_WORK=/tmp/apisix-native-response
python3 example/native-response-phase/prepare.py \
  --directory "$NATIVE_WORK" \
  --patch example/native-response-phase/native-response.patch

docker build -f example/native-response-phase/Dockerfile.toolchain \
  -t apisix-native-response-toolchain:local example/native-response-phase

docker run --rm --entrypoint sh \
  --mount "type=bind,src=$NATIVE_WORK,dst=/src" \
  --mount "type=bind,src=$PWD/example/native-response-phase,dst=/demo,readonly" \
  apisix-native-response-toolchain:local /demo/build-nginx.sh

docker build -f "$PWD/example/native-response-phase/Dockerfile.runtime" \
  -t apisix-native-response:prototype "$NATIVE_WORK"

docker run -d --name apisix-native-response-prototype \
  -p 127.0.0.1:19090:9080 apisix-native-response:prototype
```

`prepare.py` 下载并校验固定 SHA256 的源码，先应用原 runtime 所需补丁，
再 dry-run/应用本实验 diff。它也准备三个当前 APISIX rockspec 要求的纯 Lua 依赖，
并把当前 checkout 的 APISIX Lua 源码复制进构建输入。
下载地址/校验值见 `prepare.py` 和 `prepare-deps.py`。
未传 `--patch` 时使用 adapter 生成同样的源码，用于继续预研修改。

编译器镜像和 runtime 基础镜像均锁定 digest；Docker build 没有网络安装 RUN 步骤。
下载沿用环境的 HTTPS proxy/系统 CA，不关闭证书校验。
若运行在本次受管云环境，实际 Docker 命令清除了继承的 endpoint/context/TLS selectors，
并显式使用 `--host=unix:///var/run/docker.sock`；没有改宿主源码权限。
两个 runtime 构建中遇到的线程池和 Lua/Lua C 模块路径差异已固化在脚本/配置中。

## 自动测试

不要同时对同一个实例发额外手工审核请求，以免干扰活动计数断言。

```bash
python3 example/native-response-phase/verify.py \
  --output /tmp/native-response-results.json

python3 example/native-response-phase/verify-resources.py \
  --output /tmp/native-response-resources.json

docker logs apisix-native-response-prototype > /tmp/native-response.log 2>&1
python3 example/native-response-phase/verify-trace.py /tmp/native-response.log \
  --output /tmp/native-response-traces.json

docker exec apisix-native-response-prototype openresty -t -p /usr/local/apisix/
```

黑盒测试预期 `PASS: 33 checks`，涵盖：

- 两种 Lua 注入；sleep timer、TCP cosocket 的挂起/恢复。
- 通过、拒绝、chunked 后半段拒绝、UTF-8、空正文、4096 字节、上游 404/503。
- 声明超限和实际超限、截断/断连、审核错误状态/非法回复/回复超限。
- C 层审核总时限、正文总时限、持续滴流、Lua 异常、禁止提前输出。
- 两阶段客户端断开，各重复 5 次。
- 规范化 URI 仍进入受保护路径。

资源测试再增加 20 次断开，比较 worker PID、FD 数及本地服务活动/取消计数。
这是有限次数的资源回收实验，不是长期内存或压力测试。

trace 检查读取真正的 C 事件日志，要求每次请求单次提交、单次清理；
挂起期间 `header_sent=0`、`header_calls=0`、请求引用计数保持 1。
Nginx 可能在释放最后的连接引用前后执行 cleanup，所以 cleanup 点允许计数为 0 或 1。
日志中的预期 error 是主动触发的 Lua 异常和提前关闭上游夹具；脚本拒绝其他 error、
alert/crit/emerg、重复发头或 worker 信号退出。

## 手工观察

```bash
curl --noproxy '*' -i http://127.0.0.1:19090/native/allow
curl --noproxy '*' -i http://127.0.0.1:19090/native/deny
curl --noproxy '*' -i http://127.0.0.1:19090/native/large-chunked
curl --noproxy '*' -i http://127.0.0.1:19090/native/timeout
curl --noproxy '*' -i http://127.0.0.1:19090/block
curl --noproxy '*' -s http://127.0.0.1:19090/stats
```

拒绝响应中 `X-Native-Phase: response`、`X-Header-Observer: 403`、
`X-Native-Upstream-Status: 201` 来自实验观测器：原生上游确实返回 201，
新阶段修改为 403，后续原 header filter 第一次运行便看到了 403。
观测头只用于本实验。

自动并发测试先观察审核已处于 active，再用裸 socket 连续 300 ms 验证没有任何下行字节，
包含状态行与头；随后普通请求必须在审核仍 active 时完成。

## 本次测量与证据

2026-10-31 UTC，Linux x86_64，Docker 28.4.0。
源码来自 OpenResty 1.27.1.2 bundle，Nginx 1.27.1、ngx_lua 0.10.28、
lua-resty-core 0.1.31；沿用 APISIX runtime 1.3.2 的依赖与所需模块。
有效配置 `worker_processes 1`；另有不接收 HTTP 请求的 privileged agent。

证据文件：`results.json`、`trace-results.json`、`resource-results.json`、
`replay-results.json`、`runtime-evidence.txt`、`trace-excerpts.txt`。
以 JSON 中的原始数值为准；主要观测为审核约 1.00 秒，普通请求约 2 ms；
审核前的 300 ms 静默窗口没有响应字节；拒绝为 403/27 字节；
审核总时限约 1.50 秒，正文收集总时限约 1.20 秒。

核心日志实际出现以下顺序，header chain 只进入一次：

```text
collect_start       header_sent=0 header_calls=0
body_complete       header_sent=0 header_calls=0
response_enter      header_sent=0 header_calls=0
yield / resume ...  header_sent=0 header_calls=0
response_leave      header_sent=0 header_calls=0
commit              header_sent=0 header_calls=0
header_chain_enter  header_sent=0 header_calls=1
cleanup             header_calls=1
```

| 检查 | 最终结果 |
| --- | --- |
| 黑盒检查 | 主实例与干净补丁重放实例各 33 项通过。 |
| 单 worker 并发 | 主实例审核 1.0023 秒，普通请求 0.0018 秒，在审核仍 active 时完成。 |
| 两种注入入口 | file 审核及 block 中 timer 恢复、202/正文修改均通过。 |
| C 层顺序 | 主实例记录 58 个 native 请求，71 次 yield、54 次 resume；所有请求至多一次进入 header chain。 |
| 取消 | 主实例 30 个请求在发送头前取消并清理，其中 20 次来自额外资源实验。 |
| 有限资源回收 | 额外 20 次断开前后 FD 均为 18，活动请求归零，worker PID 未变。 |
| 复现一致性 | 25 个 native 源码文件逐字节一致；干净重编译的 Nginx 二进制 SHA256 也与主实验完全相同。 |

原型二进制 SHA256：
`1464f5ce70a668517668b1c5401490b0cdaa37745c3f830057f73383a9dc4b4f`。

导出补丁后还在新目录重放，25 个 native 源码文件与实验树逐字节相同，
重新编译并运行独立实例验证。构建过程中曾遇到旧软件源和 vfs 构建缓存占满磁盘，
最终使用固定 compiler 镜像并清理本轮未用缓存后重新测试，未把这些环境错误当作方案成功或失败。
旧 Lua 接管 Demo 的结果不混入上述证据。

## 尚未确认的部分

本结论只覆盖本地配置中的 native HTTP proxy 小文本路径。后续若采用该方向，
至少还需要单独设计和验证：

- cache hit、store、X-Accel/internal redirect、error_page、子请求和非 proxy 响应源。
  这些路径并未统一接入；本 Demo 不启用它们，不能把此补丁当成全路径审核开关。
- HTTP/2/3、gRPC、升级协议、压缩、流式响应、大正文、磁盘缓冲和下行背压。
- ngx.thread、semaphore、UDP、pipe、worker-thread、自定义 Lua on_abort 等更多异步 API。
  本次只扩展 TCP/sleep/协程所需入口；不通过全局白名单误开放其他 API。
  独立 `ngx.timer.at` 也没有变成父请求所属的回调机制。
- 多插件顺序、缓存/实体头规则、重试与统计语义、keepalive 优化。
  原型关闭受审核业务 upstream 的连接复用；修改正文的接口并不自动定义所有实体头规则。
- 高并发、长期内存稳定性、OOM、worker 退出/reload，以及跨版本/二进制 ABI 兼容。
  C timer 约束事件循环可继续调度的异步等待，不保证抢占无限 Lua 计算。

以上保留为后续采用决策和生产设计的输入，没有在预研阶段扩展成完整框架。
没有运行 APISIX 全量回归，也没有发布到外部服务。
Draft PR 用于保存本次研究证据，不表示产品实现、兼容性或合并条件已经满足。

清理实验实例：

```bash
docker rm -f apisix-native-response-prototype
```

源码工作树、镜像和材料仍保留，可用于继续预研或重新构建。
