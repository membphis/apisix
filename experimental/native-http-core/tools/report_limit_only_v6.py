#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate the frozen-v6 limit-count-only experiment and render its results."""
import argparse
import json
from pathlib import Path
import re
from statistics import mean


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    directory = parser.parse_args().directory
    environment = json.loads((directory / "environment.json").read_text())
    rows = json.loads((directory / "results.json").read_text())
    assert len(rows) == 6
    assert [(r["round"], r["mode"]) for r in rows] == [
        (i, mode) for i, order in enumerate(environment["orders"], 1) for mode in order]
    assert environment["enabled_plugins"] == ["limit-count"]
    assert environment["global_plugins"] == [] and not environment["metrics_scrape"]
    assert not environment["prometheus_enabled"]
    assert environment["module_sha256"] == environment["reference_module_sha256"]
    assert environment["module_sha256"] == "6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8"
    by = {(r["round"], r["mode"]): r for r in rows}
    assert len(by) == 6
    resources_reference, config_reference = None, None
    changes, table, latency = [], [], []
    for row in rows:
        assert row["duration_seconds"] == 60 and row["error_rate"] == 0 and row["prefix"] == "limit"
        assert len(row["workers"]) == 1
        sample = directory / f"round-{row['round']}" / row["mode"]
        config = json.loads((sample / "config.yaml").read_text())
        native = json.loads((sample / "native.json").read_text())
        resources = json.loads((sample / "apisix.json").read_text())
        audit = json.loads((sample / "configuration-check.json").read_text())
        assert config["plugins"] == native["plugins"] == native["local_conf"]["plugins"] == ["limit-count"]
        assert config["plugin_attr"]["prometheus"]["enable_export_server"] is False
        assert native["local_conf"]["plugin_attr"]["prometheus"]["enable_export_server"] is False
        assert not re.search(r"\blisten\s+[^;\n]*\b9091\b", (sample / "nginx.conf").read_text())
        assert audit["global_plugins"] == [] and not audit["key_auth_enabled"] and audit["ai_plugins_enabled"] == []
        assert not audit["prometheus_enabled"] and not audit["metrics_scrape"] and audit["prometheus_port_closed"]
        assert resources["global_rules"] == [] and resources["consumers"] == []
        for route in resources["routes"]:
            assert set(route["plugins"]) == ({"limit-count"} if route["id"] == "limit" else set())
        if resources_reference is None:
            resources_reference, config_reference = resources, config
        assert resources == resources_reference and config == config_reference
        latency.append(f'| {row["round"]} | {row["mode"]} | {row["latencies_us"]["p99"] / 1000:.3f} '
                       f'| {row["rss_end_bytes"] / 1048576:.2f} |')
    for index in range(1, 4):
        lua, native = by[index, "lua"], by[index, "native"]
        delta = 100 * (native["qps"] / lua["qps"] - 1)
        changes.append(delta)
        table.append(f'| {index} | {" → ".join(environment["orders"][index - 1])} '
                     f'| {lua["qps"]:,.2f} | {native["qps"]:,.2f} | {delta:+.2f}% '
                     f'| {lua["worker_cpu_us_per_request"]:.2f} | {native["worker_cpu_us_per_request"]:.2f} |')
    qps = {mode: mean(by[i, mode]["qps"] for i in range(1, 4)) for mode in ["lua", "native"]}
    cpu = {mode: sum(by[i, mode]["worker_cpu_seconds"] for i in range(1, 4)) * 1e6 /
           sum(by[i, mode]["requests"] for i in range(1, 4)) for mode in ["lua", "native"]}
    overall = 100 * (qps["native"] / qps["lua"] - 1)
    summary = {"rounds": 3, "sample_seconds": 60, "enabled_plugins": ["limit-count"],
               "global_plugins": [], "prometheus_enabled": False, "metrics_scrape": False,
               "paired_qps_changes_percent": changes, "qps_mean": qps,
               "ratio_of_mean_qps_change_percent": overall, "pooled_cpu_us_per_request": cpu,
               "pooled_cpu_change_percent": 100 * (cpu["native"] / cpu["lua"] - 1),
               "all_c_samples_faster": all(x > 0 for x in changes), "observed_error_rate": 0}
    (directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    provenance = '[版本与复现](PROVENANCE.md)。\n\n' if (directory / 'PROVENANCE.md').is_file() else ''
    report = f'''# 原 v6 核心：仅 limit-count，关闭全局 Prometheus

{provenance}日期：{environment['started_utc']}。复测同一历史 C 二进制，未引入后续候选优化。
原版三轮平均 {qps['lua']:,.2f} QPS，C 平均 {qps['native']:,.2f} QPS，均值之比变化 **{overall:+.2f}%**。
逐轮变化范围 {min(changes):+.2f}%–{max(changes):+.2f}%。六样本错误率均为 0。

| 轮次 | 顺序 | 原版 QPS | C QPS | 变化 | 原版 CPU μs/请求 | C CPU μs/请求 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(table)}

总 CPU 秒/总请求数：原版 {cpu['lua']:.2f} μs/请求、C {cpu['native']:.2f} μs/请求，变化 {summary['pooled_cpu_change_percent']:+.2f}%。

## 配置与方法

- 双方启用列表严格为 `["limit-count"]`；`global_rules=[]`、`consumers=[]`。没有 Prometheus、key-auth 或 AI 插件。
- `/limit/*` 仅 limit-count；保留上一场景的四个 URI 模式，其他路由插件为空，以控制路由匹配结构。
- Prometheus exporter 显式关闭，9091 不监听且 TCP 连接被拒绝；没有指标抓取。旧准备器的启动横幅写死了“global Prometheus :9091”，该横幅不是实际状态，配置与端口断言才是证据。
- C 模块 SHA-256 为 `{environment['module_sha256']}`，与上次带全局 Prometheus 的三轮一致。
- 冻结源码来自 `a3770f02b3adc2c295e993b8234da3e49f26864c`；仅准备器增加 exporter=false，runner 清空全局规则/限制插件并将旧采样器的抓取回调置空。
- 旧 sampler 的其余逻辑、报文、每秒进程/CPU/RSS 观测与错误检查保持一致；无效 apikey header 保留，不启用认证。
- local、constant key `demo`、1,000,000,000/600 秒，全部压力请求经过计数后转发到真实后端；限流响应头已预检。
- 单网关 worker CPU 0、上游 CPU 1、wrk CPU 2–4，2 线程/32 连接、HTTP/1.1 keepalive，后端返回 3 字节 `ok`。
- 各样本重启网关，预热 5 秒、正式 60 秒；顺序 Lua→C、C→Lua、Lua→C。六份资源与本地配置逐一比较完全一致。
- worker 替换、非 2xx/3xx、socket error 或新增 error/alert 会让样本失败。完成后恢复仅 limit-count 的 native 配置，额度 5/60 秒。

## 延迟与 RSS

| 轮次 | 实现 | p99 ms | 结束 RSS MiB |
| --- | --- | ---: | ---: |
{chr(10).join(latency)}

共享云环境三次配对不提供统计置信区间；闭环 p99 处于各实现各自达到的吞吐，不是固定到达率延迟。
本结果只对应上述历史 v6 二进制/源码，不用于证明分支后续候选代码的性能。
完整配置、预检记录、原始 wrk、environment.json、results.json 和 summary.json 在本目录。
'''
    (directory / "REPORT.md").write_text(report)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
