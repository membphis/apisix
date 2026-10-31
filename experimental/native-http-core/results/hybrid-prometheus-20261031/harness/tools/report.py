#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Render measured data; never substitute the earlier routing prototype results."""
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
RESULTS = HERE / "results"
PREFIXES = ("plain", "auth", "limit", "combined")
NAMES = {"plain": "全局 Prometheus", "auth": "+ key-auth", "limit": "+ local limit-count",
         "combined": "+ key-auth + local limit-count"}
MODULE = "/opt/apisix-core-module/lib/ngx_http_apisix_core_module.so"


def read(directory):
    rows = json.loads((directory / "results.json").read_text())
    assert len(rows) == 8 and all(row["duration_seconds"] == 60 for row in rows)
    assert all(row["error_rate"] == 0 for row in rows)
    indexed = {(row["prefix"], row["mode"]): row for row in rows}
    assert len(indexed) == 8
    for prefix in PREFIXES:
        assert (prefix, "lua") in indexed and (prefix, "native") in indexed
    return indexed, json.loads((directory / "environment.json").read_text())


def main():
    rows, environment = read(RESULTS)
    for relative, expected in environment["native_sources_sha256"].items():
        assert hashlib.sha256((HERE / relative).read_bytes()).hexdigest() == expected, \
            f"Recorded measurement does not match current native source: {relative}"
    assert hashlib.sha256((HERE / "runtime.lua").read_bytes()).hexdigest() == \
        environment["hashes"]["/src/experimental/native-http-core/runtime.lua"], \
        "Recorded measurement does not match the current coroutine bridge"
    previous = None
    if (RESULTS / "before-optimization" / "results.json").is_file():
        previous, old_environment = read(RESULTS / "before-optimization")
        assert environment["hashes"][MODULE] != old_environment["hashes"][MODULE], \
            "Final results still refer to the initial binary; wait for the new matrix."
    throughput, latency, initial = [], [], []
    qps_changes, cpu_changes, rss_changes = [], [], []
    for prefix in PREFIXES:
        lua, native = rows[prefix, "lua"], rows[prefix, "native"]
        delta = (native["qps"] / lua["qps"] - 1) * 100
        cpu_delta = (native["worker_cpu_us_per_request"] / lua["worker_cpu_us_per_request"] - 1) * 100
        qps_changes.append(delta); cpu_changes.append(cpu_delta)
        rss_changes.append((native["rss_end_bytes"] / lua["rss_end_bytes"] - 1) * 100)
        throughput.append(f'| {NAMES[prefix]} | {lua["qps"]:,.0f} | {native["qps"]:,.0f} | {delta:+.1f}% '
                          f'| {lua["worker_cpu_us_per_request"]:.2f} | {native["worker_cpu_us_per_request"]:.2f} '
                          f'| {cpu_delta:+.1f}% |')
        if previous is not None:
            old_lua, old_native = previous[prefix, "lua"], previous[prefix, "native"]
            old_delta = (old_native["qps"] / old_lua["qps"] - 1) * 100
            initial.append(f'| {prefix} | {old_lua["qps"]:,.0f} | {old_native["qps"]:,.0f} | {old_delta:+.1f}% |')
        for mode, row in (("Lua", lua), ("C", native)):
            peak = max(row["rss_peak_bytes"], row["rss_start_bytes"], row["rss_end_bytes"])
            latency.append(f'| {prefix} | {mode} | {row["latencies_us"]["p50"] / 1000:.3f} '
                           f'| {row["latencies_us"]["p99"] / 1000:.3f} '
                           f'| {row["rss_start_bytes"] / 1048576:.2f} '
                           f'| {row["rss_end_bytes"] / 1048576:.2f} | {peak / 1048576:.2f} |')
    body = '''# standalone HTTP C 核心 demo：实测结果

日期：__DATE__（UTC）。源码基线：`b31e828c6f6b588ae954bf69c8432c08fb73bb7e`。
本报告属于新的 `ngx_http_apisix_core_module`，没有使用早期 267 行局部路由实验的性能数据。

__FINDING__

## 验收范围

按用户最后确认，只实现并验收本次场景使用的核心机制：standalone HTTP、C 请求阶段/路由/上下文/插件计划、
Consumer 与配置合并、单个静态上游及动态连接复用、错误处理和指标。
key-auth 与 local limit-count 在指定前缀上启用，Prometheus 全局启用。Lua 插件业务源码保持不变。
健康检查、慢启动、服务发现、其余协议和 SDK 不作为本次验收条件；扩展点及限制见 [SCENARIOS.md](SCENARIOS.md)。

## 功能与内存证据

- 同一源码和运行时，使用 APISIX CLI 生成共同的 NGINX 配置模板。native 仅替换生命周期/阶段入口、C 代理开关，
  并将模板的转发头预处理交给等价的 C 请求准备逻辑。HTTP 传输、连接池参数、日志和资源配置保持一致。
- 两种实现各通过 34 个真实请求，状态指标精确增量均为 21×200、3×404、6×401、2×429、1×503、1×504。
  覆盖前缀边界、URI 规范化、请求体/HEAD、header/query 认证、隐藏凭证、消费者配额隔离、超时和错误。
- 同一套场景另在 native 通用回调桥接下通过。直接 C 过滤链保留 APISIX Server/上游状态头，
  并验证带 Lua 过滤回调的配置会失败关闭，不能静默跳过回调。
- 另用原版与 native 的真实 Prometheus 标签验证：日志变量批量准备保留已缓存的只读 status、
  false 值，自定义 getter 仍在插件实际读取时执行。
- 双 worker 的共享配额均为 5 个接受、27 个拒绝。实际后端连接 ID 验证两种实现均复用上游连接。
- native 另通过上游不可用、客户端断连、插件异常失败关闭及原版 Lua 核心加载守卫检查。
  守卫还检查已加载函数的源码位置，拒绝从原版非插件 Lua 核心取得实现。
- 六组核心回归共 1,955 次 HTTP 检查，另有 SDK 断言；覆盖合法 yielding、参数/返回值、nil/false、脚本、路由、DNS、
  一致性哈希、身份合并、自动 reader、配置失败回退和旧快照回收。缓存热命中保持合并配置对象身份。
- 相同回归与场景差分通过 ASan/UBSan。仅新模块被 instrument；运行时和第三方 DSO 未 instrument，关闭 leak detection。
  这不是长期泄漏或生产稳定性的证明。完整上游 APISIX Test::Nginx 测试未声称通过。

验证输出：[优化构建](results/verification-optimized.log)、[ASan/UBSan 构建](results/verification-sanitizers.log)。

## 最终一分钟样本

每条路由各采集 Lua、C 一个样本，每样本 60 秒，预热 5 秒，无额外自动重复轮次。
全程启用全局 Prometheus，每秒抓取一次；压测配额为 1,000,000,000/600 秒，使用有效认证 key。
四场景共八个最终样本均无观测到的请求错误、worker 替换或新增网关 error/alert 日志。

| 场景 | Lua QPS | C QPS | 吞吐变化 | Lua CPU μs/请求 | C CPU μs/请求 | CPU/请求变化 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
__THROUGHPUT__

| 前缀 | 实现 | p50 ms | p99 ms | 起始 RSS MiB | 结束 RSS MiB | 采样峰值 RSS MiB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
__LATENCY__

延迟来自 wrk 闭环，分别处于各实现实际达到的吞吐，不能据此断言固定到达率下的 p99 改善。
CPU/请求只计网关 worker，包含 NGINX 与 Lua，不计上游、压测端和 privileged agent。

## 优化记录

__HISTORY__

独立 LuaJIT CPU 采样显示 C/解释执行及 GC 占比较高，`pack` 出现在解释执行与 GC 热点中；
代码检查发现身份合并逐请求复制配置并编译计划。随后优化：

1. 通用 Lua 调用桥接直接转发 varargs，保留完整返回值数量、nil 和 false，移除临时 `pack` 表。
2. 缓存已加载模块，请求准备直接读取 NGINX 变量并使用底层 header 原语。
3. Consumer 合并配置与执行计划按配置代际缓存，保留最多 256 个组合，超出后使用请求内计划。
4. 无回调的过滤阶段由 C 完成核心处理后直接返回，减少语言边界调用。

优化后重新跑完整功能和内存检查，再重新采集以上八个最终样本。
CPU 采样不是 native C 函数级火焰图，不据此分配某个函数的精确耗时百分比。

### 按用户建议直接调用第三方 C API

__NATIVE_HISTORY__
这代表当时实现的开销，不代表 C 架构的性能上限。
早期极简实验省略过部分核心职责，保留了原 Lua SDK，不能把其 40%–50% 提升当作完整实现的理论上限。

本轮保留插件与场景，进一步修改：

1. `upstream_response_time`、header/connect timings 直接调用
   `ngx_http_lua_var_ffi_upstream_response_time(r, buf, type)`，保留重试后的计时聚合与数值返回类型。
   状态、请求耗时、主机和地址使用对应专用 C 原语；普通变量走 NGINX 变量 API。
2. 变量写入、标量请求/响应头、选点、超时、重试和 keepalive 直接调用原有导出 C API，移除 Lua/FFI 中转。
   多值 header 的现有处理仍保留。连接池 key 和参数与上一版相同。
3. C 将完成回调、选取下一项及阶段结束合成一次转换；薄 Lua 桥接只执行回调和传输返回值。
   已由 C 发布的缓存值可直接回传，C 继续控制 cache admission、失效和不可缓存 getter。
4. 独立 GCC 函数剖析发现组合场景每请求约设置八次上下文元数据；现按配置代际和消费者身份缓存。
   请求上下文和限流诊断表预分配容量，减少扩容。剖析输出见
   [profile-functions-before-metadata-cache.log](results/profile-functions-before-metadata-cache.log)。

函数剖析只用于查找 C 子树热点，包含 instrumentation 开销，不是网关 CPU 百分比，也不作为吞吐数字。
新增变量差分、数值类型、缓存 false、不可缓存 getter、请求头变量失效和限流 JSON 断言通过。
本轮最终数字均在无剖析、无 sanitizer 的优化构建上重新采集。

### 直接 NGINX 过滤阶段与 C JSON 编码

仅直接调用第三方 C API 的完整矩阵保存在
[before-native-filters/](results/before-native-filters/)，包括源码快照和当时的二进制。
其基础场景 +2.3%、认证 -0.6%、限流 -3.2%、组合 -10.5%，没有将这些数字当作最终上限。

- 本次配置的三个原版插件均没有 header/body 回调。移除这两个空阶段的 ngx_lua 入口，
  由 NGINX C 过滤链完成核心头处理并传递 body buffers。该模式拒绝 Lua 过滤回调、脚本和 tracing；
  通用回调桥接作为独立路径保留并做同样的场景差分。
- 函数剖析中的另一热点是限流诊断 JSON 的多层 Lua 表构造。固定窗口、可精确表示的整数形状
  现在直接用 C 编码；NGINX 变量保留完整 JSON，读取时再生成 Lua 字符串，避免逐请求创建中间表。
  非该形状继续走原有通用编码路径。
- 编码回归覆盖引号、反斜线、换行、NUL、UTF-8、false/null、缺失字段、允许/拒绝/错误、
  小数和数值精度边界，并与通用 table/cjson 路径比较解码后的完整对象，验证连续更新不会读到旧缓存。

变量读取、固定窗口编码和配置缓存都是 C 实现。未改写任何 Lua 插件业务代码，未关闭全局 Prometheus。

### 日志阶段批量准备变量

前一轮完整数据、源码及二进制保存在
[before-log-prefetch/](results/before-log-prefetch/)。该轮认证、限流及组合吞吐分别变化 +7.4%、+7.6%、+2.9%，
基础场景仍为 -4.4%。这组结果也完整保留。

本轮将日志阶段需要的稳定 HTTP/指标字段在 C 中一次取齐，减少 Lua 插件逐字段往返 SDK 的次数。
已有缓存值优先，包含 false；已注册的自定义 getter 保留按需执行；tracing 开启时保留原读取路径。
字段读取、类型转换、缓存判断均在 C 中，Lua 只读取 C 准备的数据。

## 环境、复现和边界

- OpenResty 1.27.1.2 / APISIX runtime 1.3.2，底图 digest 固定在 Dockerfile；两边使用同一组依赖。
- 新模块 GCC 10、`-O2`，测量前拒绝带 ASan/UBSan 符号的二进制。NGINX、Lua 插件和依赖相同。
- 直接 C 适配层依赖固定 runtime 的导出 ABI；升级运行时需核对签名、重编译和回归。
  压测与 host runner 也拒绝 GCC 函数剖析构建。
- 单网关 worker 固定 CPU 0，上游 worker 固定 CPU 1，wrk 在 CPU 2–4；采样前验证 worker affinity。
- wrk 2 线程、32 连接、HTTP/1.1 keepalive，3 字节 `ok` 响应。共享云环境，不是独占硬件。
- 原始输出、环境/依赖、源码与二进制 SHA-256、每个样本的配置位于 [results/](results/)。
  该轮新模块 SHA-256：`__MODULE_HASH__`。
- `python3 experimental/native-http-core/tools/dev.py` 是完整本地回归入口；
  `tools/run.py test` 做两种实现的场景差分，`tools/run.py bench` 重新采样并恢复 native 五请求演示配额。

这是一个可复现的场景 demo。单样本数据不提供统计置信区间；一分钟 RSS 和无崩溃观察不证明长期稳定性优于原版。
不声称已完整重写或兼容 APISIX 的全部核心能力。
'''
    finding = (f'本次最终 C 吞吐变化为 {min(qps_changes):+.1f}% 至 {max(qps_changes):+.1f}%，'
               f'CPU/请求变化为 {min(cpu_changes):+.1f}% 至 {max(cpu_changes):+.1f}%。'
               f'结束 RSS 变化为 {min(rss_changes):+.1f}% 至 {max(rss_changes):+.1f}%。')
    if max(qps_changes) < 0:
        finding += '**本轮未证实 C 改写能提高吞吐或降低 CPU 成本，不能宣称已达到性能提升目标。**'
    elif min(qps_changes) < 0:
        finding += '**本轮收益随场景变化，尚不能宣称全面领先原版。**'
    history = '当前结果目录未包含首轮归档，仅展示本次八个样本。'
    if previous is not None:
        history = '''首轮 C 实现虽通过功能检查，但吞吐低于原版。该轮八个一分钟样本完整保存在
[before-optimization/results.json](results/before-optimization/results.json)，没有丢弃或混入最终数字。

| 前缀 | 首轮 Lua QPS | 首轮 C QPS | C 吞吐变化 |
| --- | ---: | ---: | ---: | ---: |
''' + '\n'.join(initial)
    for key, value in {"__DATE__": datetime.now(timezone.utc).strftime("%Y-%m-%d"), "__FINDING__": finding,
                       "__THROUGHPUT__": "\n".join(throughput), "__LATENCY__": "\n".join(latency),
                       "__HISTORY__": history,
                       "__NATIVE_HISTORY__": ('上一轮报告中的 C 吞吐仍低 10.3%–21.0%；完整矩阵和二进制保存在\n'
                           '[before-native-access/](results/before-native-access/)。'
                           if (RESULTS / "before-native-access/results.json").is_file()
                           else '上一轮报告中的 C 吞吐仍低 10.3%–21.0%；当前结果目录不含那一轮归档。'),
                       "__MODULE_HASH__": environment["hashes"][MODULE]}.items():
        body = body.replace(key, value)
    (HERE / "RESULTS.md").write_text(body)
    print("\n".join(throughput))


if __name__ == "__main__":
    main()
