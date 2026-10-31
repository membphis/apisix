# 本次核心成本专项的版本与证据

来源为冻结 `a3770f02b3adc2c295e993b8234da3e49f26864c`，Lua源码基线 `b31e828c6f6b588ae954bf69c8432c08fb73bb7e`。
沿用此前 limit-count-only 的配置和原插件源码，切换为固定8,000请求/秒负载，不能跨负载比较绝对CPU/请求或延迟。

原模块 SHA-256：`6e8e9df0057dccfab6a34785a7bb9fc836c8ab71fdcbf2424beefdaacb2687a8`。
control/gc-only 使用原模块；profile/timing 的 Native 使用独立的 native-core-cost.so，
它只给原 native_phases.c、module.c 的直接过滤链与清理加边界，源码在 diagnostic-module-source/。
原主分支C核心、runtime.lua、Lua插件和常规压测工具未修改，原实测二进制未覆盖。

- harness-combined/：第一批工件二进制与测量脚本；source-combined/：对应源码。
- harness-separated/、source-separated/：拆分探针后工件/源码。analyze.py 只生成报告，不影响运行样本。
- 各样本 artifacts.json/summary.json 记录实际工具/二进制哈希，正式运行结束时再次校验未变。
- 每个case目录保存实际配置、原始负载、CPU/计量数据和error.log；八个正式case全部60秒、480,000完成请求。
- calibration-*.json 是正式流量结束后独立LuaJIT进程测得的空边界，不是吞吐样本，不包含真实请求业务。
- *verify*/attribution-probes.json 含人工plugin/SDK CPU与上游等待的归属核验。人工探针在正式样本关闭。
- core-cost-driver-verify.log 与0755Z日志是未通过的预检；修正/正式数据分别保留，失败数据未用于结论。

当前scope时钟包装仍会影响JIT。仅计时相对控制的worker CPU变化两版相差约6.56μs/请求，
与核心原始观测节省约5.71μs/请求同量级。因此未得到可信的未插桩纯核心净倍率；报告中的18%/20%不可作此声明。
GC采样估计为整个VM，不能精确归给核心；scope分配计数是触发当时的VM申请，不是GC成本归属。
本工具尚未直接测量单次GC暂停分布，也不穷尽NGINX VM入口/后台核心成本。

实验结束已恢复原模块与仅limit-count配置，Prometheus关闭，未保留临时计量路由或PRELOAD。
分支的后续候选实现与本历史版本不同，本报告不验证那些候选；详见主目录其他独立研究报告。
