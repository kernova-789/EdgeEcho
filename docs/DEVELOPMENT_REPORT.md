# EdgeEcho 开发报告

日期：2026-10-28；版本：实施前方案 v1。

## 1. 目标与交付

在本目录开发独立 C++ 数据回放软件，直接理解 EdgeSense Replay 的数据库，提供 Linux / Windows 桌面界面和可脚本化命令行。所有源码、依赖副本、构建、测试数据和验证报告只写入 EdgeEcho；参考项目只读，不链接其整个工程、不更改其数据库。

桌面工作流：打开数据库或含 replay.db 的目录 → 选择传感器/指标/单位 → 设置 UTC 区间 → 查看曲线和诊断事件 → 播放/暂停/停止/跳转/倍速 → 查看记录详情和导出 CSV。内置演示数据便于第一次运行。首版聚焦离线分析，不接硬件、不部署服务、不实现诊断算法。

## 2. 当前源码调查

参考项目：`/home/u-yoimiya/文档/EdgeSense Replay`。调查时其文档已有未提交修改，必须保留。

- `src/store.cpp`：SQLite application_id=1163088464，当前 user_version=3；records 和 event_history 为主要输入。
- `include/edgesense_replay/types.hpp`：UTC 微秒，区间 `[start,end)`；kind=0 缺失、1 标量、2 字节块；quality=0 可用，其余位表示 invalid/missing/uncertain_time/partial。质量不是正常/异常结论。
- 系列身份使用 `(sensor,type,unit)`，不能混合单位。`rate` 可空；timestamp=0 是合法默认值，不代表系统生成了采样时间。
- 记录按 `(ts,seq)` 排序；索引为 records_series_time / records_time。避免 OFFSET、全库 BLOB 加载。
- event_history 按 id 先取最后版本，再判断撤销及区间；不能直接展示 events 中的原始创建版本，不能先筛选区间再选最新。
- metadata 是 UTF-8 字节长度前缀键值编码，绝不是 JSON。
- 现有库目标包含 openat/pread/fsync 等 POSIX 文件操作；直接复用整个 target 无法保证原生 Windows，因此新建小型只读适配层，复制 SQLite amalgamation 到本目录。

实际数据编码参考 `mems_driver/tests/hardware/longrun_20261026/replay_live_import.py`、`sound/replay_sound_session.py`、传感器 IIO 布局：

| 编码 | 解码策略 |
| --- | --- |
| 标量 | 使用 scalar 原值 |
| iio-le-24-v1 | 24 字节 scan，X/Y/Z 为偏移 0/2/4 的 s16 LE |
| iio-le-16-v1 | 16 字节 scan，sound 为偏移 0 的 s32 LE |
| iio-le-72-v1 | 72 字节特征 scan，28 个16位通道及 startup_flags；温度通道6有符号 |
| s16le / s32le / f32le / f64le | 明确命名的单通道紧凑波形 |
| 其他 | 元数据可查、原始字节可导出，禁止猜测编码 |

IIO raw/sound 显示的块内时间使用记录 UTC 锚点加名义采样率重建，必须标明估计，不冒充传感器逐样本时钟。特征 scan 使用记录时间，不要求 rate；count 不自动换算 g、℃ 等单位。

## 3. 平台与技术选择

核心：C++17 标准库、SQLite C API、CMake；界面：Qt 6 Widgets / Concurrent，QPainter 自绘曲线，不依赖商业图表组件。提供关闭 GUI 的构建开关。使用标准库 chrono / filesystem / 互斥与 Qt 事件循环；不使用 epoll、io_uring、Linux IIO 或平台专用网络设施。

POSIX 是接口标准，Windows 原生并不完整实现。以 C++ 标准库和 Qt 实现用户要求的跨平台目的，比引入 unistd/poll 更可靠。文件路径统一通过 UTF-8 与 filesystem 转换，Windows 路径不按本地 ANSI 编码读取。

## 4. 数据安全与会话语义

首版只接受**已停止写入并完成 checkpoint 的离线 replay.db 或正式备份中的独立数据库文件**。检查同名 `-wal`，存在非空 WAL 时明确拒绝并说明如何关闭存储连接/生成备份。不得通过忽略 WAL 显示不完整历史。

在此明确离线约束下，SQLite 使用 `mode=ro&immutable=1` 打开，禁止创建/迁移/修复/写入原库，也不创建 WAL/SHM；不在原文件夹写入日志或设置。每次查询前检查源文件大小/mtime 与 WAL，发生变化后要求重开；这不是对并发写库的支持，调用者必须遵守离线约束。只读事务固定会话快照。遇到未知 schema/version/app_id 明确拒绝。

导出与演示文件仅在用户选择的输出位置写入；本次开发和验证全部在 EdgeEcho 内执行。输出不能覆盖输入库及其 WAL/SHM，导出失败不能伪装成功。

## 5. 模块

- `include/edgeecho/core.hpp`：公共类型、数据库读取/曲线/事件接口、播放时钟、编码器接口。
- `src/database.cpp`：RAII、身份校验、绑定参数查询、键集分页、预算与取消、CSV 导出。
- `src/codec.cpp`：小端解码、通道枚举、非有限值及截断校验。
- `src/playback.cpp`：单调时间驱动虚拟时钟，暂停/重播/seek/倍速；不把壁钟调时当作播放经过时间。
- `app/`：Qt 桌面界面；后台单工作流读取，过期请求取消/丢弃，界面线程只绘制有限数据。曲线游标推进使用已加载窗口，窗口越界再请求下一段。
- `tools/`：inspect / export / demo / benchmark 命令行，方便无人值守验证。
- `tests/`：真实源库生成的兼容测试、合成边界数据、播放时钟、编码、只读、预算、GUI 冒烟。

## 6. 性能策略与边界

- 数据库页按 `(ts,seq)` 键集翻页；先检查字段长度与单条/单页字节预算，再复制 BLOB。默认每条最多 8 MiB、每页16 MiB、最多512条，不允许首条绕过预算。
- 曲线流式扫描，按时间桶保留 min/max 及其原始时间，输出与视口点数同阶；不把全部样本留在内存，不以简单抽点掩盖尖峰。
- 每请求限制扫描记录数/解码样本数及执行时间，提供取消标记；超限明确提示缩短区间，不能静默给出截断曲线。
- 质量非零/缺失值不作为好数据绘图；曲线中断，保留记录详情供核查。不同单位及通道不混画。
- 跨越起点的波形块参与曲线；标准记录导出保持 ts 属于 `[start,end)`，语义在界面与文档说明。
- 目录统计不读取 BLOB，结果系列数量有界；全库精确计数可能随数据库大小增长，后台执行并允许取消。
- 多传感器通过选择系列切换，首版单曲线避免不同单位误叠加。未知编码仍可检查与原始导出。

## 7. 开发顺序

1. 先落地本报告并固定接口。
2. 实现核心与 SQLite 只读适配、编码、时钟；并行开发 GUI 与命令行。
3. 增加演示库、自动化核心与界面测试，验证与实际 EdgeSense Replay 写出文件的兼容性。
4. Linux Release 与 sanitizer 验证；可用时进行 Windows 交叉编译，另提供 Windows/Linux CI。没有 Windows 实机时明确保留实际运行验收项。
5. 更新使用手册、实际结果与性能数据，交付可执行程序与启动方法。

## 8. 验收

功能：打开真实 v3 库；按完整系列选取；标量与已有3种 IIO 编码可看；未知编码可导出；事件修订/撤销正确；暂停、跳转、倍速、末尾重播正确；CSV 中标识、时间、质量、rate、原始字节可追溯。

正确性：重复 timestamp 分页无重/漏，半开边界一致，预算不被超大首条绕过，非零质量不连成有效曲线，时间运算无溢出，缺失 rate 不虚构，带 WAL 的库拒绝，源库文件哈希及目录内容不变。

资源：合成大样本工作负载记录实际耗时与常驻内存，不写未经测量的吞吐承诺。GUI 长查询不阻塞主线程，关闭时取消并收尾线程。

平台：Linux 编译/核心测试/Qt offscreen 冒烟；Windows CI 编译与核心测试，必要时 MinGW 交叉编译。交叉编译不等于 Windows 实机运行验收。

## 9. 官方依据

- SQLite WAL 与只读限制：https://www.sqlite.org/wal.html
- SQLite 事务隔离：https://www.sqlite.org/isolation.html
- Qt 后台执行：https://doc.qt.io/qt-6/qtconcurrentrun.html

本文是开发前方案；最终完成范围、性能及未验证项另记 `docs/VALIDATION.md`，不把计划当作已验证结果。
