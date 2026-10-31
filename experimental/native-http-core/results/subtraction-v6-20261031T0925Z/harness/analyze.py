#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Analyze worker-CPU subtraction and positive-k plugin amplification."""
import argparse
from datetime import datetime, timezone
from itertools import product
import json
from pathlib import Path
from statistics import mean


def pct(after, before):
    return (after / before - 1) * 100 if before else None


def regression(points):
    xs, ys = zip(*points)
    mx, my = mean(xs), mean(ys)
    slope = sum((x - mx) * (y - my) for x, y in points) / sum((x - mx) ** 2 for x in xs)
    intercept = my - slope * mx
    residuals = [y - (intercept + slope * x) for x, y in points]
    total = sum((y - my) ** 2 for y in ys)
    return {
        'intercept_us': intercept, 'slope_us_per_iteration': slope,
        'r_squared': 1 - sum(r * r for r in residuals) / total if total else None,
        'points': [{'k': x, 'observed_us': y, 'predicted_us': intercept + slope * x,
                    'residual_us': r, 'relative_residual_percent': r / y * 100}
                   for (x, y), r in zip(points, residuals)],
        'max_abs_relative_residual_percent': max(abs(r / y * 100) for (_, y), r in zip(points, residuals))
    }


def fit(points):
    result = regression(points)
    loo = []
    for index, (x, y) in enumerate(points):
        other = regression(points[:index] + points[index + 1:])
        predicted = other['intercept_us'] + other['slope_us_per_iteration'] * x
        loo.append({'k': x, 'predicted_us': predicted, 'observed_us': y,
                    'relative_prediction_error_percent': (predicted - y) / y * 100})
    result['leave_one_out'] = loo
    result['loo_max_abs_relative_prediction_error_percent'] = max(abs(p['relative_prediction_error_percent']) for p in loo)
    return result


def analyze(root):
    rows = json.loads((root / 'results.json').read_text())
    env = json.loads((root / 'environment.json').read_text())
    plan = json.loads(Path(__file__).with_name('analysis-plan.json').read_text())
    expected = [c['id'] for c in env['plan']]
    assert len(rows) == len(expected) == 18
    assert [r['id'] for r in rows] == expected
    for row in rows:
        assert row['duration_seconds'] == 60
        assert row['requests'] == row['load']['completed'] == row['load']['target_requests'] == env['rate'] * 60
        assert not row['load']['errors'] and row['error_rate'] == 0
        assert row['one_upstream_per_request'] and row['load']['all_slots_completed']
        assert row['load']['schedule_drained_within_one_second']
    assert not any(env[key] for key in ['clock_probes', 'allocator_probes', 'gc_probes'])
    assert env['normal_gc']
    by_id = {r['id']: r for r in rows}
    cpu = lambda key: by_id[key]['cpu_us_per_request']
    baselines = {}
    for mode in ['nginx', 'lua', 'native']:
        prefix = mode if mode == 'nginx' else mode + '-none'
        before, after = cpu(prefix + '-before'), cpu(prefix + '-after')
        baselines[mode] = {'before_us': before, 'after_us': after, 'mean_us': mean([before, after]),
                           'drift_percent': (after - before) / mean([before, after]) * 100}
    ng = baselines['nginx']['mean_us']
    addons = {m: baselines[m]['mean_us'] - ng for m in ['lua', 'native']}
    sensitivity = []
    for n, l, c in product(*[[baselines[m][key] for key in ['before_us', 'after_us']]
                             for m in ['nginx', 'lua', 'native']]):
        a, b = l - n, c - n
        sensitivity.append({'nginx_us': n, 'lua_us': l, 'native_us': c,
                            'lua_addon_us': a, 'native_addon_us': b,
                            'addon_cpu_change_percent': pct(b, a) if a > 0 and b >= 0 else None})
    fitted = {m: fit([(k, cpu(f'{m}-loop-{k}')) for k in [1, 2, 4, 8]]) for m in ['lua', 'native']}
    controls = {}
    for m in ['lua', 'native']:
        controls[m] = {
            'original_us': cpu(m + '-original'), 'loop1_us': cpu(m + '-loop-1'),
            'loop1_vs_original_percent': pct(cpu(m + '-loop-1'), cpu(m + '-original')),
            'empty_callback_us': cpu(m + '-loop-0'),
            'original_minus_no_plugin_us': cpu(m + '-original') - baselines[m]['mean_us'],
            'empty_callback_minus_no_plugin_us': cpu(m + '-loop-0') - baselines[m]['mean_us'],
            'fit_intercept_minus_empty_callback_us': fitted[m]['intercept_us'] - cpu(m + '-loop-0')
        }
    thresholds = plan['diagnostic_thresholds']
    warnings = []
    for m, b in baselines.items():
        if abs(b['drift_percent']) > thresholds['baseline_drift_abs_percent']:
            warnings.append(f'{m}: baseline drift exceeds 5%; subtraction is sensitive to time/order/environment.')
    for m, f in fitted.items():
        linear = (f['max_abs_relative_residual_percent'] <= thresholds['fit_max_abs_relative_residual_percent']
                  and f['loo_max_abs_relative_prediction_error_percent'] <= thresholds['fit_loo_max_abs_relative_prediction_error_percent'])
        f['passes_linearity_diagnostics'] = linear
        if not linear:
            warnings.append(f'{m}: positive-k points do not support a clean linear split at the diagnostic thresholds.')
        if abs(controls[m]['loop1_vs_original_percent']) > thresholds['loop1_vs_original_abs_percent']:
            warnings.append(f'{m}: loop k=1 differs from the unmodified plugin by more than 5%; injection/JIT/order effects matter.')
    for r in rows:
        if r['worker_cpu_utilization'] > thresholds['worker_utilization_fraction']:
            warnings.append(r['id'] + ': worker CPU exceeds 70% of one core.')
    if any(a <= 0 for a in addons.values()):
        warnings.append('Nonpositive addon estimate: do not interpret its relative speedup.')
    return {
        'generated_utc': datetime.now(timezone.utc).isoformat(), 'analysis_plan': plan,
        'environment': env, 'total_requests': sum(r['requests'] for r in rows),
        'baselines': baselines, 'addon_us': addons,
        'addon_cpu_change_percent': pct(addons['native'], addons['lua']) if min(addons.values()) > 0 else None,
        'baseline_sensitivity': sensitivity,
        'no_plugin_saved_us': baselines['lua']['mean_us'] - baselines['native']['mean_us'],
        'original_saved_us': cpu('lua-original') - cpu('native-original'),
        'original_cpu_change_percent': pct(cpu('native-original'), cpu('lua-original')),
        'enabled_path_savings_minus_no_plugin_savings_us':
            (cpu('lua-original') - cpu('native-original')) - (baselines['lua']['mean_us'] - baselines['native']['mean_us']),
        'fits': fitted, 'controls': controls,
        'slope_cpu_change_percent': pct(fitted['native']['slope_us_per_iteration'], fitted['lua']['slope_us_per_iteration']),
        'warnings': warnings, 'rows': rows,
        'pure_core_self_time_established': False, 'core_caused_gc_separately_identified': False,
        'statistical_confidence_intervals_established': False
    }


def report(a):
    b, f, c = a['baselines'], a['fits'], a['controls']
    reduction = a['addon_cpu_change_percent']
    ranges = [s['addon_cpu_change_percent'] for s in a['baseline_sensitivity'] if s['addon_cpu_change_percent'] is not None]
    unstable = any(abs(v['drift_percent']) > a['analysis_plan']['diagnostic_thresholds']['baseline_drift_abs_percent'] for v in b.values())
    baseline_note = ('**前后基线存在明显漂移，尚未稳定识别精确的纯核心净收益。附加成本均值只是点估计，不能忽略敏感性范围。**'
                     if unstable else '基线未触发漂移警告；附加整合成本仍不能等同于核心函数 self time。')
    lines = [
        '# 历史 v6：无请求探针的减法与插件放大实验', '',
        '[版本与工件](PROVENANCE.md)；[可复现方法](../../tools/subtraction/README.md)。', '',
        '使用 worker 进程 CPU/完成请求，正常 GC 保持开启；不插入逐请求时钟、allocator、GC profiler 或 SDK wrapper。',
        f'18 个正式样本各 60 秒，均为 {a["environment"]["rate"]:,} 请求/秒、32 连接，共 {a["total_requests"]:,} 个请求；HTTP/连接错误为零，每请求恰好一次上游。', '',
        f'本轮原插件场景 worker CPU 变化 {a["original_cpu_change_percent"]:+.2f}%；下面的减法和拟合用于解释成本。',
        baseline_note, '',
        '## 1. 纯 NGINX 与无插件 APISIX 的减法', '',
        '| 实现 | 前置 CPU μs/请求 | 后置 CPU μs/请求 | 均值 | 漂移 |',
        '| --- | ---: | ---: | ---: | ---: |'
    ]
    for mode in ['nginx', 'lua', 'native']:
        v = b[mode]
        lines.append(f'| {mode} | {v["before_us"]:.3f} | {v["after_us"]:.3f} | {v["mean_us"]:.3f} | {v["drift_percent"]:+.2f}% |')
    lines += ['', f'减去纯 NGINX 均值后，Lua 附加成本 **{a["addon_us"]["lua"]:.3f} μs/请求**，C 为 **{a["addon_us"]["native"]:.3f} μs/请求**。']
    if reduction is not None:
        lines += [f'附加成本 CPU 变化 {reduction:+.2f}%；枚举前后基线得到 {min(ranges):+.2f}% 至 {max(ranges):+.2f}% 的敏感性范围，**这不是置信区间**。']
    lines += [
        '这个差值覆盖 APISIX 整合成本，包括路由、上下文、VM 进出、动态 upstream 适配、worker 后台任务与正常 GC；',
        '不能称为 C/Lua 核心函数自身的精确耗时，也不能外推为启用插件时全部核心的成本。', '',
        '## 2. 原插件与空回调控制', '',
        '| 实现 | 原插件 CPU μs/请求 | 循环 k=1 | 相对原插件 | 空回调 k=0 | 空回调减无插件 |',
        '| --- | ---: | ---: | ---: | ---: | ---: |'
    ]
    for mode in ['lua', 'native']:
        v = c[mode]
        lines.append(f'| {mode} | {v["original_us"]:.3f} | {v["loop1_us"]:.3f} | {v["loop1_vs_original_percent"]:+.2f}% | {v["empty_callback_us"]:.3f} | {v["empty_callback_minus_no_plugin_us"]:+.3f} |')
    lines += [
        '', f'原插件场景 C 相对 Lua 的 worker CPU 变化 **{a["original_cpu_change_percent"]:+.2f}%**，节省 {a["original_saved_us"]:.3f} μs/请求。',
        f'无插件节省 {a["no_plugin_saved_us"]:.3f} μs/请求；两种节省量之差 {a["enabled_path_savings_minus_no_plugin_savings_us"]:.3f} μs/请求，',
        '表示启用插件后增加的实现差异；包含调度、SDK、VM/GC 交互，不等于插件业务代码的独立 CPU。',
        '以上是同一组观测的代数分解，不是独立的因果验证。循环版本改变返回/循环形状，必须参考 k=1 与原插件的差异。', '',
        '空回调与无插件是不同配置/执行形状，其差可能为负；不能把差值当成单条调用指令的独立耗时。', '',
        '| 原插件场景的代数拆分 μs/请求 | Lua | C |',
        '| --- | ---: | ---: |',
        f'| 纯 NGINX 基线 | {b["nginx"]["mean_us"]:.3f} | {b["nginx"]["mean_us"]:.3f} |',
        f'| 无插件 APISIX 附加成本 | {a["addon_us"]["lua"]:.3f} | {a["addon_us"]["native"]:.3f} |',
        f'| 启用插件新增路径 | {c["lua"]["original_minus_no_plugin_us"]:.3f} | {c["native"]["original_minus_no_plugin_us"]:.3f} |',
        f'| 总计（原插件 worker 实测） | {c["lua"]["original_us"]:.3f} | {c["native"]["original_us"]:.3f} |', '',
        '“启用插件新增路径”含业务、插件调度、核心 SDK 和它们引起的 VM/GC/NGINX 变化，不能全部归给插件业务。',
        '因此这张表覆盖整段 worker CPU，却不能唯一回答“所有核心（含 SDK）与纯插件业务各多少”。', '',
        '## 3. 正 k 的线性拟合', '',
        '`CPU(k) = a + b × k`，仅用 k=1、2、4、8。k=0 没有速率响应头/首次执行，作为独立控制保留。', '',
        '| 实现 | 截距 a μs | 斜率 b μs/次 | R² | 最大相对残差 | 留一预测最大误差 | 线性诊断 |',
        '| --- | ---: | ---: | ---: | ---: | ---: | --- |'
    ]
    for mode in ['lua', 'native']:
        v = f[mode]
        lines.append(f'| {mode} | {v["intercept_us"]:.3f} | {v["slope_us_per_iteration"]:.3f} | {v["r_squared"]:.5f} | {v["max_abs_relative_residual_percent"]:.2f}% | {v["loo_max_abs_relative_prediction_error_percent"]:.2f}% | {"通过" if v["passes_linearity_diagnostics"] else "未通过"} |')
    lines += [
        '', f'C 相对 Lua 的增量斜率变化 {a["slope_cpu_change_percent"]:+.2f}%。',
        '斜率包括重复的 rate_limit、SDK、响应头写入、GC 和循环/JIT 效应；截距是外推，不能直接命名为核心耗时。',
        '重复发生在同一个请求内，后续调用可复用上下文/配置缓存；其增量不等同于多个独立请求各执行一次。',
        '若假设两版插件业务成本接近一致，斜率差可以提示底层可重复路径节省，但本实验不能独立验证该假设。', '',
        'k 按升序执行，k 与运行时间共变；即使 R² 较高、残差通过，基线漂移仍可能影响斜率，不能据此证明因果收益。', '',
        '## 4. 全部 CPU 与端到端 Latency', '',
        '| 样本 | worker CPU μs/请求 | 单核利用率 | send→body均值 μs | send→body p99 μs | scheduled→body p99 μs |',
        '| --- | ---: | ---: | ---: | ---: | ---: |'
    ]
    for row in a['rows']:
        wire, scheduled = row['load']['wire_latency_us'], row['load']['scheduled_latency_us']
        lines.append(f'| {row["id"]} | {row["cpu_us_per_request"]:.3f} | {row["worker_cpu_utilization"] * 100:.1f}% | {wire["mean"]:.2f} | {wire["p99"]:.2f} | {scheduled["p99"]:.2f} |')
    lines += [
        '', '这是固定到达速率的成本/延迟测试，QPS 相同，不是吞吐上限复测。客户端保留迟到 slot 并处理正常 keepalive 关闭。',
        'p99 不可相减来分解 NGINX、插件、核心；进程 CPU 不含上游等待，但含 worker 的 NGINX/内核执行。',
        '纯 NGINX、Lua 无插件、C 无插件发送到上游的请求头逐项一致；动态与静态 upstream 的执行机制仍有差异。', '',
        '## 5. GC 与证据限制', '',
        '正常 GC 已自然进入进程 CPU 和延迟，未单独记录其核心因果成本或 pause 分布；不能把 GC 再加到上述 CPU。',
        '每个正 k/原插件组合只有一个一分钟样本；基线有前后两次，交错 Lua/C 顺序减少单向漂移，仍不能构造可信统计置信区间。',
        '该方法不增加请求计量探针，但插件放大版本确实改变工作量/JIT。诊断阈值在矩阵运行中固定，非严格事前注册，不能当显著性检验。', '',
        '## 6. 诊断警告', ''
    ]
    lines += ['- ' + w for w in a['warnings']] if a['warnings'] else ['未触发预设诊断警告；这不等于已证明精确的纯核心/GC 归因。']
    lines += ['', '下一步若继续：把 NGINX/Lua/C 基线安排为紧邻交错配对，随机化或反转 k 顺序，检查启动/JIT 和时间漂移；',
              '保留本轮全部样本，不用后来更有利的一组替换它。仍需承认仅凭减法不能唯一拆出插件业务与核心 SDK、核心因果 GC。']
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', type=Path)
    args = parser.parse_args()
    result = analyze(args.results)
    (args.results / 'analysis.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
    (args.results / 'REPORT.md').write_text(report(result))
    print(json.dumps({key: result[key] for key in ['total_requests', 'addon_us', 'addon_cpu_change_percent',
                                                 'original_cpu_change_percent', 'slope_cpu_change_percent', 'warnings']}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
