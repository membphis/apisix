# ai-proxy 单 CPU 流式吞吐基准

测量 APISIX `ai-proxy` 插件在 OpenAI Chat Completions 流式协议（SSE passthrough）下，单 worker 钉死一个 CPU 核能达到的 token/s 上限。

## 这是什么

一套独立的端到端基准测试，由三部分组成：

```
                taskset -c 3-7              taskset -c 0                taskset -c 1-2
              ┌─────────────┐  HTTP    ┌──────────────────┐  HTTP    ┌──────────────┐
              │  bench      │─ N 条 ─▶ │  APISIX worker   │─ N 条 ─▶ │  bench       │
              │  client     │ 长连接   │  workers=1       │ 长连接   │  server      │
              │  (Go)       │◀ SSE ───│  ai-proxy:openai │◀ SSE ─── │  (fake LLM)  │
              └─────────────┘          └──────────────────┘          └──────────────┘
                     │                          │
                     └── 每秒采样 ─────▶  pidstat APISIX worker CPU%
                     │
                     ▼
                bench-result.csv  +  bench-report.html
```

- **bench server**（Go）：fake OpenAI 流式上游，预编码 `chat.completion.chunk` 一段 ~200 字节的 JSON，在热循环里只做 `Write + Flush`，确保自己绝不成为瓶颈。
- **bench client**（Go）：开 N 个并发 SSE 长连接打 APISIX，按 `data:` 行计数 token，记录 inter-event 延迟到 HdrHistogram。
- **harness**（bash）：用 docker 起 APISIX（standalone yaml mode，无 etcd），`taskset` 把 worker 钉死在指定核，用 `pidstat` 采 APISIX/server/client 三方 CPU%，输出 CSV。

## 前置依赖

| 工具 | 版本 | 用途 |
|---|---|---|
| Docker | 20+ | 跑 `apache/apisix:dev`（拉 master 镜像） |
| Go | 1.21+ | 构建 `bench` 二进制 |
| `taskset` | util-linux | 绑核（CPU 隔离） |
| `pidstat` | sysstat | 采 CPU% |
| passwordless sudo | — | 让 `sudo taskset -cp` 不交互 |
| 主机端口 | 9080 / 9180 / 1981 | APISIX 数据面 / admin / fake server |
| 主机 CPU 核 | ≥ 4 | APISIX × 1，server × 1，client × 2（默认拓扑） |

> 默认拓扑下 `CLIENT_CORES=3-7` 假设 ≥ 8 核；4 核机器请用下文的 env 覆盖。

## 快速开始

从 APISIX 仓库根目录运行（不是从 `benchmark/ai-proxy/`）：

```bash
bash benchmark/ai-proxy/run.sh
```

默认 7 个并发档位 × 60 s 测量 × 5 s warmup × 5 s 冷却 ≈ **8 分钟**。

跑完后产物全部在 `benchmark/ai-proxy/result/`：

```
result/
├── bench               # 编译好的 Go 二进制
├── bench-result.csv    # 主输出：每并发档位一行
├── server.log          # bench server 启动日志
├── apisix-error.log    # 容器内 APISIX error.log 快照
└── env.txt             # 跑时的 CPU 型号 / 内核 / git sha / 参数
```

如果机器只有 4 核（VM、笔记本）：

```bash
APISIX_CORE=0 SERVER_CORES=1 CLIENT_CORES=2-3 bash benchmark/ai-proxy/run.sh
```

## 自定义

所有可调参数通过环境变量覆盖：

| 变量 | 默认 | 含义 |
|---|---|---|
| `DURATION` | `60` | 每档测量窗口秒数 |
| `WARMUP` | `5` | 每档预热秒数（数据丢弃） |
| `CONCURRENCY` | `1,2,4,8,16,32,64` | 并发档位列表，逗号分隔 |
| `APISIX_CORE` | `0` | APISIX worker 钉到哪个核 |
| `SERVER_CORES` | `1-2` | bench server 允许使用的核范围 |
| `CLIENT_CORES` | `3-7` | bench client 允许使用的核范围 |

例如，只跑 c=1/8/64 三档、每档 30 s：

```bash
DURATION=30 CONCURRENCY=1,8,64 bash benchmark/ai-proxy/run.sh
```

要扩到更高并发：

```bash
CONCURRENCY=1,2,4,8,16,32,64,128,256 bash benchmark/ai-proxy/run.sh
```

## CSV 输出格式

`bench-result.csv` 列：

| 列 | 含义 |
|---|---|
| `concurrency` | 并发流数 |
| `duration_s` | 测量窗口秒数 |
| `total_tokens` | 窗口内收到的总 SSE 事件数 |
| `total_t_per_s` | 平均吞吐（tokens / s） |
| `apisix_cpu_pct` | APISIX worker CPU%（pidstat） |
| `server_cpu_pct` | bench server CPU% |
| `client_cpu_pct` | bench client CPU% |
| `p50_inter_event_us` | 单事件间延迟中位数（µs） |
| `p99_inter_event_us` | 单事件间延迟 p99（µs） |

判读关键：**`apisix_cpu_pct` 接近 100%** 是数据可信的前提——否则瓶颈不在 APISIX，吞吐不是真上限。

## 实测结果

参考实测（Intel Xeon Skylake VM 4 核、APISIX `apache/apisix:dev`、master 的 ai-providers/ai-protocols/ai-transport 模块）：

| concurrency | tokens/s | APISIX CPU | p50 µs | p99 µs |
|---:|---:|---:|---:|---:|
| 1  | 19,480 | 99.9% |    40 |    211 |
| 2  | 18,702 | 99.9% |    94 |    343 |
| 4  | 18,480 | 99.9% |   193 |    765 |
| 8  | 18,272 | 99.9% |   391 |  1,736 |
| 16 | 17,204 | 99.9% |   817 |  4,493 |
| 32 | 14,382 | 99.9% | 1,995 |  9,814 |
| 64 | 14,136 | 100%  | 4,214 | 11,321 |

- **单核峰值约 19,500 tokens/s**（concurrency=1）
- **高并发下约 14,000 tokens/s**（concurrency=64）
- APISIX CPU 全程 99.9–100%——瓶颈在 APISIX worker 自己，cjson decode 和 OpenResty 协程调度主导单事件成本（每事件约 50 µs）
- 吞吐随并发单调下降是预期的：每加一条流就多一轮调度开销

HTML 版（含柱状/折线图）：[`benchmark/ai-proxy/bench-report.html`](./bench-report.html)，直接浏览器打开，无外部依赖。

完整设计稿和分析：[`docs/superpowers/specs/2026-05-14-bench-ai-proxy-stream-throughput-design.md`](../../docs/superpowers/specs/2026-05-14-bench-ai-proxy-stream-throughput-design.md)

## 验收检查（自动）

`run.sh` 跑完会用 `python3` 走一遍 sanity check 并打印 WARN：

- 峰值档 `apisix_cpu_pct ≥ 90%`——否则瓶颈不在 APISIX，数据不能作上限报
- 每档 `server_cpu_pct < 60%` 且 `client_cpu_pct < 60%`——否则载入端在饱和
- 吞吐到峰值前单调递增——否则可能档位选得不够宽，需要扩展 `CONCURRENCY`

## 工作原理（要点）

### APISIX 配置

`conf/config.yaml.tpl`：

```yaml
deployment:
  role: data_plane                 # data_plane + yaml = standalone（不连 etcd、无 admin API）
  role_data_plane:
    config_provider: yaml
nginx_config:
  worker_processes: 1              # 关键：单 worker
  http:
    enable_access_log: false       # 关 access log 减干扰
plugins:
  - ai-proxy
  - prometheus                     # 必须：ai-proxy 内部 require 了 prometheus.exporter
                                   # （inc/dec_llm_active_connections）。不挂任何路由，
                                   # 仅为让 prometheus-metrics shdict 被生成。
```

> 关于 prometheus：`role: traditional` 下加 `config_provider: yaml` 不会真正进入 standalone
> ——`apisix.enable_admin: true`（默认 true）会让 `config_yaml.lua` 跳过文件路由加载；必须
> 用 `role: data_plane`。同时，`ai-proxy` 在请求生命周期里调用 `prometheus.exporter` 的活动
> 连接计数函数，如果 prometheus 插件没注册，shdict 不会被声明，运行时报错。把它列在 `plugins:`
> 里但不放进任何 route/global\_rule，prometheus 的 access/log 阶段不会触发——只有那两个
> shdict inc/dec 调用作为 ai-proxy 自身成本被计入，这是设计内的。

`conf/apisix.yaml.tpl`：

```yaml
routes:
  - id: 1
    uri: /v1/chat/completions
    plugins:
      ai-proxy:
        provider: openai
        auth:
          header:
            Authorization: "Bearer sk-bench"
        override:
          endpoint: "http://127.0.0.1:1981/v1/chat/completions"
#END
```

`#END` 是 APISIX yaml 加载器要求的完结标记。

### docker 调用

```bash
docker run -d --name apisix-bench \
  --network host \    # APISIX :9080 / :9180 在宿主回环，无 NAT 损耗
  --pid host \        # 容器内 worker PID 等同宿主 PID，便于 taskset/pidstat
  -v $PWD/benchmark/ai-proxy/conf/config.yaml.tpl:/usr/local/apisix/conf/config.yaml:ro \
  -v $PWD/benchmark/ai-proxy/conf/apisix.yaml.tpl:/usr/local/apisix/conf/apisix.yaml:ro \
  apache/apisix:dev
```

### 找 worker PID

宿主上可能有其他 nginx（系统 nginx、其他 openresty 实例）——直接 `pgrep -f 'nginx: worker process'` 可能撞到无关进程。harness 用 `docker top apisix-bench` 限定到容器：

```bash
WORKER_PID="$(docker top "$APISIX_CONTAINER" -o pid,cmd | awk '/nginx: worker process/{print $1; exit}')"
sudo taskset -cp 0 "$WORKER_PID"
```

### bench client 测量周期

每个并发档位 6 个 phase：

1. 起 N 个 goroutine，每个开一条 SSE 长连接
2. `warmup` 秒——流量在跑但计数被 atomic 标志门控住，不计入
3. atomic 标志翻转，并行起 3 个 `pidstat` 子进程（采 APISIX/server/client）
4. `duration` 秒测量窗口
5. atomic 标志翻回，取消所有 goroutine，等它们退出
6. 合并每 goroutine 的本地计数和 HdrHistogram → 写一行 CSV

per-goroutine token 计数是普通 `int++`（不是 atomic），因为只有该 goroutine 写、`wg.Wait()` 提供 happens-before。这样每事件不付 LOCK 总线开销。

## 故障排查

### `apisix_cpu_pct = 0`

worker PID 拿错了——可能是宿主上有别的 nginx 抢了 `pgrep` 第一名。harness 已用 `docker top` 限定容器，理论上不会再撞到。如果还有，运行 `docker top apisix-bench` 确认实际 PID，再手动 `pidstat -p <PID> 1 5` 看活跃度。

### 端口被占

```
docker: Error response from daemon: ... port is already allocated
```

`ss -tlnp | grep -E '9080|9180|1981'` 找出占用方。常见冲突：宿主系统 nginx 占 80（不冲突）但 OpenResty 默认配置可能监听更多端口，旧测试残留的 bench server 占 1981。

### `sudo: A terminal is required`

passwordless sudo 没配。`sudo visudo` 加：

```
yourname ALL=(ALL) NOPASSWD: ALL
```

或者每次跑前手动 `sudo -v` 缓存凭据。

### `client_cpu_pct > 60` 警告

bench client 跑紧了。如果机器核够，扩 `CLIENT_CORES`（如 `2-7`）。如果就是 4 核机：APISIX 已 100% 饱和的情况下，client 紧一点不影响 APISIX 的天花板数字。

### `WARN pidstat ... only N samples (<20 may be unreliable)`

`DURATION` 太短。`pidstat -p PID 1 N` 跑 `N=int(DURATION)` 次，丢首样后剩 `N-1`。`DURATION ≥ 21` 才能稳过这个门槛。冒烟测试用 `DURATION=3` 时这个 WARN 是预期的，可以忽略。

## 目录结构

```
benchmark/ai-proxy/
├── README.md                   # 本文件
├── bench-report.html           # 实测结果可视化报告（独立 HTML）
├── run.sh                      # 主入口
├── bench/                      # Go 源码
│   ├── go.mod / go.sum
│   ├── main.go                 # subcommand router
│   ├── server.go               # fake OpenAI 上游
│   ├── server_test.go
│   ├── client.go               # 压测客户端 + CSV writer
│   └── client_test.go
├── conf/
│   ├── config.yaml.tpl         # APISIX 主配置（standalone）
│   └── apisix.yaml.tpl         # APISIX 路由（yaml-loaded）
└── result/                     # 运行时产物（已 .gitignore）
    ├── bench-result.csv
    ├── env.txt
    ├── server.log
    └── apisix-error.log
```

## 不在范围

为了让"单核 ai-proxy 上限"这个数字干净可信，本基准刻意不测：

- 多 worker 横向扩展（`worker_processes: auto`）
- 协议转换（Anthropic ↔ OpenAI 等 converter 路径）
- `ai-proxy-multi`、`ai-rag` 等同族插件
- 其他 provider（anthropic / bedrock / vertex-ai / gemini）
- 与其他插件（rate-limit / waf 等）组合的吞吐
  （prometheus 虽列在 `plugins:` 里，但没挂到任何 route，不在数据路径上——见上文配置说明）
- 大上下文长输出对 `contents` 累加 `table.concat` 的 O(n²) 影响
- TLS 上游（HTTPS 到 LLM provider 的握手 / TCP 开销）

需要测这些场景请独立写新档位脚本，复用 `bench/` 二进制即可。
