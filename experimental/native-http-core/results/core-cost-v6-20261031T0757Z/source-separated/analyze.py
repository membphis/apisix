#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Report inner-core observations separately from GC estimates and real latency."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    root = parser.parse_args().directory
    data = {name: json.loads((root / name / 'summary.json').read_text())
            for name in ['lua-control', 'native-control', 'lua-profile', 'native-profile',
                         'lua-timing', 'native-timing', 'lua-gc', 'native-gc']}
    for record in data.values():
        assert record['requests'] == 480000 and record['duration_seconds'] == 60 and record['rate'] == 8000
        assert record['load']['all_slots_completed'] and not record['load']['errors']
        assert record['original_module_sha256'] == '6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8'
    result = {}
    core_rows, gc_rows, memory_rows, latency_rows, tax_rows = [], [], [], [], []
    for mode in ['lua', 'native']:
        control, timed, gc, full = [data[mode + '-' + kind] for kind in ['control', 'timing', 'gc', 'profile']]
        scope = timed['scope']
        assert scope['completed'] == scope['plugin_entries'] == 480000 and scope['sdk_entries'] == 4320000
        assert scope['invalid'] == scope['missing'] == scope['pending'] == 0
        assert scope['sampled_completed'] == 7500 and not scope['allocator_enabled'] and not scope['gc_sampling_enabled']
        assert gc['scope']['completed'] == 0 and not gc['scope']['allocator_enabled'] and gc['scope']['gc_sampling_enabled']
        calibration = json.loads((root / ('calibration-' + mode + '.json')).read_text())
        assert calibration['completed'] == 10000 and calibration['invalid'] == calibration['missing'] == 0
        raw = scope['core_cpu']['mean_ns'] / 1000
        blank = calibration['core_cpu']['mean_ns'] / 1000
        gc_fraction = gc['scope']['gc_cpu_samples'] / gc['scope']['total_cpu_samples']
        gc_cpu_est = gc['worker_cpu_us_per_request'] * gc_fraction
        alloc = full['scope']['allocator']
        assert not any(row['failed'] for row in alloc)
        result[mode] = {
            'core_cpu_raw_us_per_sampled_request': raw,
            'empty_shape_cpu_us': blank,
            'core_cpu_approx_after_empty_us': raw - blank,
            'core_sample_cpu_total_ns': scope['core_cpu']['sample_total_ns'],
            'core_cpu_window_extrapolated_seconds_raw': raw * 480000 / 1e6,
            'core_active_wall_us': {key: scope['core_active_wall'][key + '_ns'] / 1000 for key in ['mean', 'p50', 'p95', 'p99']},
            'vm_gc_cpu_fraction_sample_estimate': gc_fraction,
            'vm_gc_cpu_us_per_request_sample_estimate': gc_cpu_est,
            'gc_effective_samples_per_worker_cpu_second': gc['scope']['total_cpu_samples'] / gc['worker_cpu_seconds'],
            'core_tagged_allocator_new_calls_per_request': alloc[1]['new_calls'] / 480000,
            'core_tagged_allocator_requested_new_bytes_per_request': alloc[1]['requested_new_bytes'] / 480000,
            'plugin_tagged_allocator_requested_new_bytes_per_request': alloc[2]['requested_new_bytes'] / 480000,
            'all_vm_requested_new_bytes_per_request': sum(row['requested_new_bytes'] for row in alloc) / 480000,
            'control_worker_cpu_us_per_request': control['worker_cpu_us_per_request'],
            'wire_latency_control_us': control['load']['wire_latency_us'],
            'scheduled_latency_control_us': control['load']['scheduled_latency_us'],
            'driver_lateness_control_us': control['load']['driver_lateness_us'],
            'observed_cpu_overhead_percent': {kind: 100 * (data[mode + '-' + kind]['worker_cpu_us_per_request'] / control['worker_cpu_us_per_request'] - 1)
                                             for kind in ['profile', 'timing', 'gc']}}
        core_rows.append(f'| {mode} | {raw:.3f} | {blank:.3f} | {raw-blank:.3f} | {scope["core_active_wall"]["p50_ns"]/1000:.3f} | {scope["core_active_wall"]["p95_ns"]/1000:.3f} | {scope["core_active_wall"]["p99_ns"]/1000:.3f} |')
        gc_rows.append(f'| {mode} | {gc["scope"]["gc_cpu_samples"]} / {gc["scope"]["total_cpu_samples"]} | {gc_fraction*100:.2f}% | {gc_cpu_est:.3f} |')
        memory_rows.append(f'| {mode} | {alloc[1]["new_calls"]/480000:.3f} | {alloc[1]["requested_new_bytes"]/480000:.2f} | {alloc[2]["requested_new_bytes"]/480000:.2f} | {sum(row["requested_new_bytes"] for row in alloc)/480000:.2f} |')
        latency_rows.append(f'| {mode} | {control["load"]["wire_latency_us"]["mean"]:.2f} | {control["load"]["wire_latency_us"]["p50"]:.2f} | {control["load"]["wire_latency_us"]["p99"]:.2f} | {control["load"]["scheduled_latency_us"]["p99"]:.2f} | {control["load"]["driver_lateness_us"]["p99"]:.2f} |')
        tax_rows.append(f'| {mode} | {control["worker_cpu_us_per_request"]:.3f} | {full["worker_cpu_us_per_request"]:.3f} | {timed["worker_cpu_us_per_request"]:.3f} | {gc["worker_cpu_us_per_request"]:.3f} |')
    result['core_raw_change_percent'] = 100 * (result['native']['core_cpu_raw_us_per_sampled_request'] / result['lua']['core_cpu_raw_us_per_sampled_request'] - 1)
    result['core_after_empty_approx_change_percent'] = 100 * (result['native']['core_cpu_approx_after_empty_us'] / result['lua']['core_cpu_approx_after_empty_us'] - 1)
    result['core_after_empty_approx_saved_us'] = result['lua']['core_cpu_approx_after_empty_us'] - result['native']['core_cpu_approx_after_empty_us']
    result['timing_worker_cpu_drift_difference_us'] = (
        data['lua-timing']['worker_cpu_us_per_request'] - data['lua-control']['worker_cpu_us_per_request']
        - data['native-timing']['worker_cpu_us_per_request'] + data['native-control']['worker_cpu_us_per_request'])
    result['clean_core_speedup_established'] = False
    (root / 'analysis.json').write_text(json.dumps(result, indent=2) + '\n')
    report = f'''# 历史 v6：核心 CPU、GC 与 Latency 专项

[版本与证据边界](PROVENANCE.md)；[计量方法](../../tools/core_cost/README.md)。

本报告区分直接插桩观测、近似扣空估计、VM GC 采样估计和未插桩端到端延迟。
纯核心相对倍率不是原吞吐变化的倒数，也不能从 worker CPU 总量直接推出。
**本次尚未获得可信的未插桩核心净收益倍率。** 仅计时相对控制的 worker CPU 变动，两版相差
{result['timing_worker_cpu_drift_difference_us']:.3f} μs/请求，与观测到的核心节省量同量级；无法排除探针和样本漂移。

## 1. 核心内执行 CPU 与活动墙钟

| 实现 | 核心原始 CPU μs/请求 | 空形状 CPU μs | 近似扣空后 μs | 核心活动墙钟 p50 μs | p95 μs | p99 μs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(core_rows)}

未扣空的插桩核心 CPU 变化 {result['core_raw_change_percent']:+.2f}%；近似扣空后变化 {result['core_after_empty_approx_change_percent']:+.2f}%，
近似节省 {result['core_after_empty_approx_saved_us']:.3f} μs/请求。
**扣空只近似消除时钟/边界税，不能消除包装函数对 JIT、调度和代码布局的影响，不能把这个倍率当成精确的未插桩核心收益。**

每版在 480,000 个完成请求中周期抽样 7,500 个（1/64），累计活动 CPU/墙钟按请求求和后统计分位数。
窗口核心总 CPU 只能按样本均值外推，原始样本累计与外推值分别保存在 analysis.json，不冒充全量直接计时。
核心 CPU 包含区间内实际发生的 GC 和所调用的同步底层原语；核心之外的网络处理/上游等待没有计入。
插件业务被扣除，插件调用核心 SDK 的时间重新计入核心；每请求恰好一次插件、九次 SDK。正常 GC 没有关闭。
Native 还计入直接 C header/body 与请求清理；调用下一个 NGINX filter 前停止计时。
空形状按 Lua 五阶段/Native 六阶段、各九次 SDK 建模；真实 body buffer/阶段入口边界可能变化。
CPU 与墙钟不是同时读取，存在读时钟顺序偏差；细小的 CPU>墙钟不能当作物理执行现象。

## 2. GC：VM 实际采样与核心分配归属

| 实现 | GC CPU样本 / 总CPU样本 | VM GC CPU占比估计 | VM GC CPU μs/请求估计 |
| --- | ---: | ---: | ---: |
{chr(10).join(gc_rows)}

GC-only 使用原核心二进制，不包装核心/插件/SDK、不拦截 allocator，只用 LuaJIT 的 CPU timer profiler。
该固定 POSIX runtime 使用 ITIMER_PROF，启动时核对请求间隔为1ms；实际信号交付/合并会改变有效采样率。
CPU μs估计为 worker CPU/请求 × GC样本比例，不用“样本数×1ms”假装精确计时。
两版有效交付采样率分别约 {result['lua']['gc_effective_samples_per_worker_cpu_second']:.1f}、{result['native']['gc_effective_samples_per_worker_cpu_second']:.1f} 样本/CPU秒，明显不同；GC比例接近，不据此证明GC改善。
**这是整个 worker Lua VM 的 GC 采样估计，不能精确归因于核心分配，不是单次 GC pause 分布。**
GC可能回收更早由插件/核心产生的对象，回收时所在scope不能等同分配来源；不能把这行再加到已包含GC的核心CPU上。

| 实现 | 核心scope新分配调用/请求 | 核心scope新申请字节/请求 | 插件scope新申请字节/请求 | VM全部新申请字节/请求 |
| --- | ---: | ---: | ---: | ---: |
{chr(10).join(memory_rows)}

分配数据来自另一次全量诊断的公开 Lua allocator 包装，按当时执行scope标记。它包含VM/JIT分配及探针影响，
不是逻辑对象数、存活堆、物理malloc或RSS，不包含NGINX池、外部FFI malloc和JIT机器码映射。
插件业务/源文件未改，插件scope分配接近是归属的交叉检查；C仍需创建插件可见的Lua对象，不能预设GC必然更低。

## 3. 实际 Latency：未插桩固定负载

| 实现 | send→body平均 μs | p50 μs | p99 μs | scheduled→body p99 μs | 客户端发起延迟p99 μs |
| --- | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(latency_rows)}

均为8,000请求/秒、32 keepalive连接、60秒、480,000完成请求；计划slot不丢弃，调度迟到仍计入scheduled延迟。
客户端支持NGINX每连接1000请求后的正常关闭/重连。延迟记录开始于计划到达或实际发送，分别报告。
这些控制样本使用原二进制和原请求代码，不加载计量库；每版一轮，云环境/客户端调度影响仍存在。
不能把本表p99差值唯一归因于某个核心函数、GC，或与核心活动墙钟p99直接相减。

## 探针扰动与证据强度

| 实现 | 未插桩 worker μs/请求 | 全量探针 | 仅核心计时 | 仅GC采样 |
| --- | ---: | ---: | ---: | ---: |
{chr(10).join(tax_rows)}

上表的差异包含观测扰动和样本间漂移，不是可直接从核心时间扣除的常数。
先发现全量探针税后，追加独立 timing-only/GC-only 样本；保留全量诊断和失败预检，没有挑选更有利样本替代。
八个正式样本均60秒、5秒预热、同样8,000请求/秒；每份480,000请求完整完成，无HTTP错误或worker替换。
归属核验包括插件人工2ms CPU、SDK人工2ms CPU、上游30ms等待，证据在对应verify目录。
此工具仅支持当前无yield的静态GET/local-limit场景；检测到交错/未闭合帧即失败，不能直接推广到任意yielding插件。

版本：冻结 `a3770f02` 的核心/Lua插件，原模块SHA为 `6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8`。
独立诊断模块只加C filter/cleanup边界；未修改主分支核心或插件源码。各样本artifacts.json记录工具/二进制哈希。
后续要更精确分离GC暂停与核心因果成本，需在匹配的LuaJIT运行时为GC step加可校准边界，或使用能正确解码JIT栈的低扰动CPU工具。
'''
    (root / 'REPORT.md').write_text(report)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
