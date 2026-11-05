# EdgeEcho 实际交付与验证

完成日期：2026-10-29，Asia/Shanghai。开发报告先于实现写入，见 [DEVELOPMENT_REPORT.md](DEVELOPMENT_REPORT.md)。本文件记录实际结果，不把计划中的 CI 或 Windows 运行当作成功证据。

## 已实现

独立 C++17 / SQLite 核心、Qt6 Widgets 桌面软件、CLI。桌面支持离线数据库/目录、完整系列身份、通道、曲线、诊断、记录详情、分页、演示库、CSV、暂停/停止/倍速/跳转。后台取消与世代检查防止过期结果替换当前视图。

已支持 scalar、missing、三种实际 IIO 编码，以及 s16le/s32le/f32le/f64le。未知编码保留检查/导出能力；波形重建时间明确标估计。没有实时硬件采集、音频设备播放、FFT或新诊断算法。

## Linux 实际验证

本机 x86_64，GCC13.3、CMake3.28.3、Qt6.4.2、SQLite3.54.1。

| 验证 | 实际结果 |
| --- | --- |
| Release CTest | 6/6通过，含Qt offscreen测试 |
| Debug ASan + UBSan核心 CTest | 5/5通过，开启泄漏检查 |
| GUI深入冒烟 | 4个系列，多个通道，9次窗口加载，暂停/8x/seek/回起点通过 |
| GUI输入安全 | 中文路径通过，原库SHA256及目录列表不变，非空WAL/损坏库明确拒绝 |
| 原存储库兼容 | 原始未修改Store实际写出3000记录+1事件；EdgeEcho逐页ID/时间/metadata、事件、曲线验证通过 |
| 参数/导出 | 严格整数、非法参数、中文及特殊字符路径、CSV记录数与不覆盖行为通过 |
| 格式边界 | 重复timestamp、半开区间、跨入起点BLOB、记录/页/事件预算、取消、源变更、schema/app_id拒绝通过 |
| 非好质量/特征 | quality断点保留尖峰；feature scan无需rate且不会错误推断每点间丢样 |
| 源日志 | 非空WAL及rollback journal均拒绝，不尝试恢复或忽略 |

源数据库读取均使用明确离线契约；哈希检查不构成对并发写入支持。GUI自动化是 offscreen 真实异步加载与渲染，不是人工长期使用验收。

机器可读结果：[GUI](evidence/gui.json)、[CLI](evidence/cli.json)、[原库兼容](evidence/compatibility.json)。构建日志及截图在 `build/Testing/Temporary/`、`build/gui-validation/`；instrumented日志在 `build-asan/Testing/Temporary/`。

## 性能实际测量

AMD Ryzen5 4600U，Linux x86_64，合成数据库248,623,104字节/200,310记录。1500时间桶，标准15秒查询上限。每项是新CLI进程；缓存未受控，不能解释成冷缓存/Windows/实板结果。表中耗时仅 `Database::plot`，进程总时长包含打开与全库系列统计。

| 数据 | 输出 | Plot耗时 | 单进程峰值RSS | 进程总时长 |
| --- | --- | --- | --- | --- |
| 8,192,000波形样本/250块 | 3000点 | 509.54ms | 30,608KiB | 0.83s |
| 200,000标量记录 | 4177点含断点 | 549.58ms | 21,784KiB | 0.85s |
| 历史尾部1秒/100条 | 100点 | 0.787ms | 21,716KiB | 0.29s |

397条非零质量标量正确排除；波形解码数量完整，源库前后SHA256一致，无sidecar。扫描完整20万记录仍为流式处理；末端跳转使用时间索引。全库overview的精确统计仍随数据库规模增长，在后台执行并有15秒预算，特别大的库可能需要进一步目录索引设计。

详细原始数据与源码/二进制指纹：[performance.json](evidence/performance.json)。可以通过 `tests/performance/run.py --reuse --output build/performance --cli build/edgeecho_cli` 只读重测已有工作负载。

## Windows 证据与保留项

MinGW-w64 x86-64 实际交叉编译已通过，产物：

- `build-windows/edgeecho_cli.exe`
- `build-windows/database_tests.exe`
- `build-windows/codec_playback_tests.exe`

`file`均识别为PE32+ x86-64；CLI静态链接工具链运行时，仅依赖KERNEL32.dll、msvcrt.dll。本机没有Windows或Wine，**没有执行这些程序，也没有构建/运行Windows Qt桌面程序**。桌面源代码共用Qt API，已提供MSVC/Qt构建步骤和Windows/Linux工作流；远端CI尚未触发，不能视作Windows实际通过。Windows系统上的GUI运行、Unicode文件对话框、平台插件部署仍需验收。

## 修改边界

所有新源码、文档、依赖解压、构建、测试数据、结果均位于 EdgeEcho。参考库的37个构建/源码/头文件/SQLite文件在兼容检查前后指纹一致；参考项目的预存未提交文档状态保留。没有提交或推送Git，没有修改存储项目、驱动项目或前端项目。

SQLite amalgamation在复制后未编辑；Qt/MinGW依赖只解压到 `.deps/`，没有系统安装动作。参见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。
