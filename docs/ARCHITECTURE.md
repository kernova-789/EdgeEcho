# 核心与数据流

桌面程序和 CLI 共用 `edgeecho_core`，通过 CMake 目标 `EdgeSense::Replay` 静态链接 Replay Core。Qt 负责桌面交互，CLI 和 Qt 使用同一个 `Database` 入口。

```text
Qt / CLI
  └── Database
        ├── Replay Store::open_offline / snapshot
        │     ├── query_series → SignalView → 曲线、探针、音频和频谱
        │     └── search_events → 有效诊断版本、确认状态和区间叠加
        └── ArchiveProjection → 系列统计、原始序号、载荷和无损 CSV

steady_clock → PlaybackClock → 当前 UTC 位置 → 游标与下一显示窗口
Replay Store::append / create_event → 离线演示库 → 原子发布
```

## 离线数据源

打开已关闭写入并完成 checkpoint 的 v3 数据库，或 Replay 正式备份中的独立数据库文件。`Store::open_offline` 校验格式、文件身份、WAL 和回滚日志，每次读取边界还会复核源文件。Qt 的每个工作任务独立打开数据库，交接时同时核对文件原生标识、数据库 ID 和保留代数。发生源文件替换或修改时，`SourceChanged` 触发界面重新打开流程。

`ArchiveProjection` 只读取 Replay 公开接口未包含的原始记录序号、原始 metadata 字节和系列总数、覆盖范围及载荷组成，保留原始表格和 CSV 格式。它使用与 Replay 同一套 SQLite，在经过 Replay 校验的源文件上建立 immutable 只读连接，并在查询前后通过 Replay 快照复核。原始页按 `(ts,seq)` 键集分页，受单条字节、页字节和 SQL 执行预算约束。

演示库由 Replay 写入私有目录，关闭写入并完成离线校验后，再经 `AtomicOutput` 发布。已有目的文件由原子发布操作保护。

## 历史读取与显示

`query_series` 按精确的设备、指标和单位分页，并包含跨入窗口左边界的块。每次扫描固定一个 Replay 快照，传入余下的执行时间和取消标志。历史查询批次独立于表格页大小，仍受页字节和总记录数预算约束。`SignalView` 借用页内记录，提供原始通道值、波形采样时间和半开窗口内的样本下标。标量和特征用记录时间，波形时间按名义采样率估算。XYZ 48 字节和 XYZS 72 字节特征各有对应的通道与物理量显示。

`plots` 让同一系列的多条曲线共用一次查询和每条记录的解码视图。所有通道共用样本和输出预算，时间桶保留极值的原时间并输出有序断点。此布局由 EdgeEcho 控制，以便保留同一时间桶内的质量中断和波形覆盖不连续。`probe` 直接找窗口内最近的有效原始样本，等距离选较早时刻，同刻沿用记录顺序。

诊断显示由 `search_events` 选出快照内的最新有效版本，并返回确认状态。EdgeEcho 为当前窗口施加累计条数和字节预算，并保持同一开始时间按记录序号排序。

## 界面并发

QtConcurrent 工作线程创建并销毁 Database，GUI 线程接收有限的 Plot、Page 和 Event 值对象。最多运行一项任务，并保留最新的待执行请求。前台请求取消旧任务，界面按 generation 丢弃过期结果。关闭窗口时会取消任务并等待退出。原始 SQL 和历史查询可以检查取消，事件检索在有执行时限的分页边界检查取消。

PlaybackClock 管理界面时间位置、暂停、跳转、倍速及窗口切换。完整原始输出由 page 和 export 提供。

## 缓存与构建验证

主窗口保留最多3个/估计32MiB结果缓存；键含SourceIdentity、完整系列、通道、半开区间和分辨率。缓存命中前后构造只读Database核对身份。预取与悬停为低优先任务，前台请求使其取消/过期。最后一个窗口使用与前台seek完全相同的`windowAt`计算，避免尾端预取键不匹配。缓存预算不等于进程RSS硬限制。

图表交互由`app/plot_widget.*`处理，变更范围后通过回调发起新查询。日历及夏令时判定由`app/time_range_editor.*`处理；UTC微秒是唯一查询时间，日历毫秒和微秒余数仅作为编辑方式。最大结束边界显示其前1微秒但不改变规范区间。

记录表采用批量更新、交互列宽和32行抽样测量；不在每次setItem后对全部512行执行ResizeToContents，避免在界面线程出现重复字体布局。

统一源码身份覆盖核心、界面、测试、构建工具和工作流。`tools/validate_current.py`为每次运行创建独立JUnit位置，核对刚配置的测试清单并拒绝空/跳过/旧结果及源变化。GUI、CLI、Linux包和Windows交叉产物绑定同一源码标识。运行证据的范围由profile单独说明，不能以交叉构建代替原生运行。
