#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate and summarize the separate three-pair limit-count recheck."""
import argparse
import json
from pathlib import Path
from statistics import mean


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    directory = parser.parse_args().directory
    environment = json.loads((directory / "environment.json").read_text())
    rows = json.loads((directory / "results.json").read_text())
    assert len(rows) == 6
    assert all(r["duration_seconds"] == 60 and r["error_rate"] == 0
               and r["prefix"] == "limit" and len(r["workers"]) == 1 for r in rows)
    assert [(r["round"], r["mode"]) for r in rows] == [
        (i, mode) for i, order in enumerate(environment["orders"], 1) for mode in order]
    assert environment["enabled_plugins"] == ["prometheus", "limit-count"]
    assert environment["module_sha256"] == environment["reference_module_sha256"]
    by = {(r["round"], r["mode"]): r for r in rows}
    assert len(by) == 6
    first_resources = None
    table, latency, changes = [], [], []
    for index in range(1, 4):
        lua, native = by[index, "lua"], by[index, "native"]
        delta = 100 * (native["qps"] / lua["qps"] - 1)
        cpu_delta = 100 * (native["worker_cpu_us_per_request"] / lua["worker_cpu_us_per_request"] - 1)
        changes.append(delta)
        order = " → ".join(environment["orders"][index - 1])
        table.append(f'| {index} | {order} | {lua["qps"]:,.2f} | {native["qps"]:,.2f} | {delta:+.2f}% '
                     f'| {lua["worker_cpu_us_per_request"]:.2f} | {native["worker_cpu_us_per_request"]:.2f} | {cpu_delta:+.2f}% |')
        for mode, row in [("lua", lua), ("native", native)]:
            location = directory / f"round-{index}" / mode
            config = json.loads((location / "config.yaml").read_text())
            native_config = json.loads((location / "native.json").read_text())
            resources = json.loads((location / "apisix.json").read_text())
            audit = json.loads((location / "configuration-check.json").read_text())
            assert config["plugins"] == native_config["plugins"] == ["prometheus", "limit-count"]
            assert native_config["local_conf"]["plugins"] == config["plugins"]
            assert config["nginx_config"]["worker_processes"] == 1
            assert not audit["key_auth_enabled"] and audit["ai_plugins_enabled"] == []
            assert resources["consumers"] == []
            assert resources["global_rules"] == [{"id": "metrics", "plugins": {"prometheus": {}}}]
            for route in resources["routes"]:
                assert set(route["plugins"]) == ({"limit-count"} if route["id"] == "limit" else set())
            if first_resources is None:
                first_resources = resources
            assert resources == first_resources, "resource configurations differ across samples"
            latency.append(f'| {index} | {mode} | {row["latencies_us"]["p99"] / 1000:.3f} '
                           f'| {row["rss_end_bytes"] / 1048576:.2f} |')
    qps = {mode: mean(by[index, mode]["qps"] for index in range(1, 4)) for mode in ["lua", "native"]}
    cpu = {mode: sum(by[index, mode]["worker_cpu_seconds"] for index in range(1, 4)) * 1e6 /
           sum(by[index, mode]["requests"] for index in range(1, 4)) for mode in ["lua", "native"]}
    overall = 100 * (qps["native"] / qps["lua"] - 1)
    previous = environment["reference_limit_qps"]
    reference_delta = 100 * (previous["native"] / previous["lua"] - 1)
    summary = {"rounds": 3, "sample_seconds": 60, "paired_qps_changes_percent": changes,
               "qps_mean": qps, "ratio_of_mean_qps_change_percent": overall,
               "mean_paired_change_percent": mean(changes), "pooled_cpu_us_per_request": cpu,
               "pooled_cpu_change_percent": 100 * (cpu["native"] / cpu["lua"] - 1),
               "original_change_percent": reference_delta,
               "difference_from_original_percentage_points": overall - reference_delta,
               "all_c_samples_faster": all(change > 0 for change in changes),
               "observed_error_rate": 0, "enabled_plugins": environment["enabled_plugins"]}
    (directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    direction = "三轮 C 吞吐均高于原版。" if summary["all_c_samples_faster"] else "各轮方向见表，不能宣称三轮均有提升。"
    version = ('版本边界：[PROVENANCE.md](PROVENANCE.md)。本复测固定于历史 C 核心，完整冻结工作树为 `a3770f02`；不用于验证分支后续候选优化。\n\n'
               if (directory / 'PROVENANCE.md').is_file() else '')
    report = f'''# limit-count：三轮独立配对复测

{version}日期：{environment['started_utc']}。{direction}
三轮原版平均 {qps['lua']:,.2f} QPS，C 平均 {qps['native']:,.2f} QPS，均值之比提升 **{overall:.2f}%**。
每轮提升范围 {min(changes):.2f}%–{max(changes):.2f}%；原一次样本为 {reference_delta:.2f}%，本轮均值之比相差 {overall - reference_delta:+.2f} 个百分点。

| 轮次 | 顺序 | 原版 QPS | C QPS | 提升 | 原版 CPU μs/请求 | C CPU μs/请求 | CPU 变化 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(table)}

按三轮总 CPU 秒/总请求数计算：原版 {cpu['lua']:.2f} μs/请求，C {cpu['native']:.2f} μs/请求，变化 {summary['pooled_cpu_change_percent']:+.2f}%。
六个样本均为 60 秒、无观测请求错误、worker 替换或新增 error/alert 日志。

## 配置与可比性

- 双方 HTTP 启用列表严格为 `prometheus`、`limit-count`。全局仅 Prometheus，`/limit/*` 路由仅 limit-count，无 key-auth 或 AI 插件、无消费者配置。
- 为保持匹配结构，保留原四个 URI 模式，其他三个路由的插件为空；每个样本实际资源配置已逐一比较，完全一致。
- 上次该路由也未执行 key-auth，但 demo 注册列表和其他路由存在 key-auth；此次进一步移除了这些配置，因此这是更严格的无认证复测。
- C 二进制与原 11.1% 实测相同，SHA-256：`{environment['module_sha256']}`；没有新增性能优化或重编译。
- 复用原 `benchmark.sample`，保留原报文（包含不会启用认证的未使用 `apikey` header）、wrk 脚本与采样口径。
- local 策略、constant key `demo`、1,000,000,000/600 秒，确保请求计数后代理到实际上游，不用 429 快速拒绝提高吞吐。
- 单网关 worker CPU 0，上游 CPU 1，wrk CPU 2–4，2 线程/32 连接、keepalive、3 字节 `ok` 响应。
- 每个样本新启网关，预热 5 秒，正式 60 秒，全局 Prometheus 每秒抓取一次。顺序 L→C、C→L、L→C。
- 每次测量前验证插件列表/资源、无 key 的原认证路径返回 200、限流响应头正确；完整配置和检查记录在对应轮次目录。
- 完成后恢复同样无认证/无 AI 的 native 演示，额度为 5/60 秒。

## 延迟与内存

| 轮次 | 实现 | p99 ms | 结束 RSS MiB |
| --- | --- | ---: | ---: |
{chr(10).join(latency)}

延迟来自各实现各自吞吐下的闭环 wrk，不代表固定到达率下的延迟差异。
这是共享云环境的三个样本对，没有给出统计置信区间，也不要求绝对 QPS 或百分比逐次相同。
原始结果见 `results.json`、环境/代码哈希见 `environment.json`、汇总见 `summary.json`。
'''
    (directory / "REPORT.md").write_text(report)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
