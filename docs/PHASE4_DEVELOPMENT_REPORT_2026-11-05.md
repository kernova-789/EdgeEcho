# EdgeEcho 第四阶段开发报告：跨平台音频回放、FFT 频谱分析与原生动画 UI

日期：2026-11-05，Asia/Shanghai；版本：0.4.0。
源码构建身份：`source-sha256:a01c7b6cd24e97cdc1663471a07211b083c7a750e20e967250b722c2ffbc270f`

---

## 1. 阶段目标与完成概况

第四阶段（`0.4.0`）围绕「声音不仅能画出图像而且能实时发声可听、支持频域 FFT 频谱分析、跨 Linux/Windows 原生系统 UI 风格、丰富动画视觉体验」四大核心诉求展开，严格保持对 `EdgeSense Replay` 离线数据库（SQLite Schema v3、`application_id = 1163088464`）的零修改、只读安全边界。

| 模块 | 完成状态 | 核心交付与工程边界 |
| --- | --- | --- |
| **跨平台实时音频回放 (`AudioPlayer`)** | 已完成 | Linux 动态加载 `libpulse-simple.so.0`（PulseAudio/PipeWire）或 `libasound.so.2`（ALSA）；Windows 原生链接 `winmm`（`waveOut`）；无音频设备或 `offscreen` 环境自动回退实时模拟步进后端。支持低采样率线性重采样（≥ 16 kHz）、播放时钟同步、跳转、音量调节（0–100%）、静音与「试听当前窗口」。 |
| **标准 WAV 音频导出 (`encode_wav` / `export_wav`)** | 已完成 | 将 `iio-le-16-v1`（24-bit 有效位宽 s32 PCM）及通用波形编码为标准 44 字节头部 RIFF WAVE 16-bit 单声道 PCM `.wav` 文件；桌面端与 CLI（`edgeecho_cli audio`）均通过 `AtomicOutput` 原子发布，拒绝覆盖已有文件。 |
| **频域 FFT 频谱分析 (`compute_spectrum`)** | 已完成 | 去直流均值 + Hann 窗函数 + 幅值补偿的 Cooley-Tukey Radix-2 FFT；桌面端支持「时域波形 / FFT 频谱图」一键切换、主频峰值自动标注与悬停频点读数；CLI 新增 `edgeecho_cli spectrum` JSON 输出。 |
| **系统原生风格 UI 重构** | 已完成 | 全面采用系统原生控件（`QMainWindow`、`QMenuBar`、`QToolBar` + `QStyle::StandardPixmap` 系统图标、`QGroupBox`、`QSplitter`、`QTabWidget`、`QSlider`、`QProgressBar`）与系统 `QPalette` 调色板；将低频微秒/日历区间编辑器收纳至底部独立标签页，显著扩大主波形/频谱视口占比。 |
| **界面与图表动画体系** | 已完成 | 1) 数据/模式切换 360ms `OutCubic` 包络扫光与频谱柱生长动画；2) 播放指针 30ms 呼吸脉冲光晕与 5 段式动态均衡器；3) 频谱主频峰值雷达扩散波纹信标；4) 悬停十字准星与读数气泡 160ms `OutQuad` 渐显；5) 底部详情面板 260ms `InOutCubic` 平滑抽屉折叠与标签页切换淡入动画；6) 实时音频 VU 动态电平表。 |

---

## 2. 架构与关键文件变更

- **[include/edgeecho/core.hpp](file:///home/u-yoimiya/文档/EdgeEcho/include/edgeecho/core.hpp)**：新增 `AudioOptions`、`AudioClip`、`SpectrumBin`、`Spectrum`、`encode_wav`、`compute_spectrum`、`Database::audio`、`Database::export_wav`、`Database::spectrum` 及跨平台 `AudioPlayer` 接口。
- **[src/audio.cpp](file:///home/u-yoimiya/文档/EdgeEcho/src/audio.cpp)**：实现 RIFF WAVE 编码、Hann 窗 FFT 频谱计算、线性重采样及 Linux（PulseAudio/ALSA `dlopen`）/ Windows（`winmm`）/ Headless（`software-null`）三态音频输出引擎。
- **[src/database.cpp](file:///home/u-yoimiya/文档/EdgeEcho/src/database.cpp)**：在离线源身份校验与预算护栏下实现 `Database::audio`、`Database::export_wav` 与 `Database::spectrum`，自动跳过非 `good` 质量记录并归一化峰值。
- **[app/plot_widget.hpp](file:///home/u-yoimiya/文档/EdgeEcho/app/plot_widget.hpp) & [app/plot_widget.cpp](file:///home/u-yoimiya/文档/EdgeEcho/app/plot_widget.cpp)**：重构为系统原生 `QPalette` 配色，新增 FFT 频谱渲染模式与 4 路流畅动画状态机，同时保持首帧同步像素可测性。
- **[app/main.cpp](file:///home/u-yoimiya/文档/EdgeEcho/app/main.cpp)**：重构原生主窗口布局、工具栏、音频控制栏、电平指示器及底部四标签页详情抽屉动画。
- **[tools/cli.cpp](file:///home/u-yoimiya/文档/EdgeEcho/tools/cli.cpp)**：新增 `audio` 与 `spectrum` 命令行子命令。
- **[tests/codec_playback.cpp](file:///home/u-yoimiya/文档/EdgeEcho/tests/codec_playback.cpp)、[tests/export_tests.cpp](file:///home/u-yoimiya/文档/EdgeEcho/tests/export_tests.cpp)、[app/interaction_tests.hpp](file:///home/u-yoimiya/文档/EdgeEcho/app/interaction_tests.hpp)**：新增 WAV 编码、40 Hz 正弦波 FFT 主频检测、音频播放器生命周期、数据库 WAV 原子导出及 GUI 音频/频谱真实组件交互回归测试（共 42 项 GUI 交互检查点）。

---

## 3. 验证与构建证据

最新全量验证报告已固化于 [docs/evidence/current-validation.json](file:///home/u-yoimiya/文档/EdgeEcho/docs/evidence/current-validation.json)：

| 验证配置 | 状态 | 结果摘要 |
| --- | --- | --- |
| **Linux Release (`release`)** | 通过 | 13 / 13 项 CTest 测试全部通过（含 `gui_smoke`、`gui_inputs` 42 项真实窗口交互、`plot_widget` 像素级断言、`export`、`codec_playback` 等）。 |
| **Linux ASan + UBSan (`asan`)** | 通过 | 9 / 9 项核心与 CLI 测试全部通过，`detect_leaks=1` 零内存泄漏、零未定义行为。 |
| **Windows Cross (`windows-cross`)** | 通过 | MinGW-w64 `x86_64-w64-mingw32-g++` 编译 `edgeecho_cli.exe` 及 5 个测试套件，PE32+ (`0x8664`) 二进制格式与内嵌 `source-sha256` 校验通过。 |
| **Linux 独立发布包 (`validate_package.py`)** | 通过 | `dist-linux/EdgeEcho-0.4.0-linux-x86_64.tar.gz` 迁移至含中文与空格路径后，CLI/GUI 版本身份、演示库生成、Inspect 与 GUI 冒烟全部通过。 |

---

## 4. 当前项目局限与下一步开发方向（0.5.0+ 路线图）

结合上游采集项目 `EdgeSense Replay` 与驱动项目 `mems_driver` 的数据契约，当前项目仍有以下深水区方向值得在下一阶段推进：

### 方向一：物理工程单位校准与 `mems_driver` 29 维特征语义字典
- **现状不足**：数据库中 `iio-le-24-v1`（振动）与 `iio-le-16-v1`（声音）存储的是 ADC 原始整型 `count`，`iio-le-72-v1` 的 29 个特征通道目前以通用编号/简写展示，尚未绑定 `mems_driver` 中的物理量纲与工程含义（如各轴 RMS、峭度 Kurtosis、偏度 Skewness、峰值因子 Crest Factor、包络能量等）。
- **下一步计划**：
  1. 解析记录 `metadata` 中的 `scale` / `sensitivity` / `offset` 字段（并支持用户在 GUI 中手动输入传感器灵敏度系数），提供**「原始 ADC Count ↔ 物理工程量（$\text{m/s}^2$、$g$、$\text{Pa}$、$\text{dB SPL}$）」**一键切换。
  2. 为 `iio-le-72-v1` 的 29 个特征通道内置完整的中文工程名称、物理含义说明与分组筛选（时域统计特征 / 频域能量特征 / 冲击包络特征）。

### 方向二：多子图联动（Multi-Pane Linked Subplots）与时频瀑布图（Spectrogram / STFT）
- **现状不足**：当前单画布支持同一振动系列的「XYZ 同屏」或单通道「时域 / FFT 频谱」切换，但工程师在排查机械异响时，往往需要**上方看三轴振动波形、中间看声音波形、下方同时看短时傅里叶变换（STFT）时频瀑布图**。
- **下一步计划**：
  1. 引入**多子图垂直分屏同步视图**，支持不同传感器、不同物理单位的通道上下同屏排列，共享同一 UTC 微秒时间轴与同步播放指针。
  2. 在单窗口 FFT 基础上增加**STFT 时频谱图（Spectrogram 热力图）**，直观呈现瞬态冲击或异响随时间演化的频率轨迹。

### 方向三：多倍速音频变速不变调（Time-Stretching）与带通滤波试听
- **现状不足**：当前支持倍速回放与音频输出，但当以 $0.25\times$ 慢放或 $4\times$ 快放查看高频异常冲击时，若能对声音信号进行变速不变调或频带滤波（仅听某一段共振频率），故障听诊体验将大幅跃升。
- **下一步计划**：
  1. 在 `AudioPlayer` 中支持跟随 `PlaybackClock` 倍速的变速重采样 / WSOLA 变速不变调算法。
  2. 支持在 FFT 频谱图上框选频段（Band-Pass Filter），实时滤除背景低频轰鸣噪声后再试听或导出 WAV。

### 方向四：超大历史库（>1000 万条记录）秒级首屏目录与后台渐进统计
- **现状不足**：`Database::overview()` 在首次打开数据库时执行全表精确 `GROUP BY` 计数，对于数 GB 乃至数十 GB 的长周期历史库，首次打开耗时随总记录数线性增长。
- **下一步计划**：
  1. 拆分「快速首屏系列发现」（利用索引快速定位传感器/指标/首尾时间戳，1 秒内打开界面并可立即播放）与「后台渐进式精确计数」。
  2. 支持将常用区间书签、校准系数与分析批注保存在独立的外部工作区文件（`.edgeecho.json`）中，依然保持源 `replay.db` 100% 只读零修改。

### 方向五：Windows 原生 Qt 实机闭环与实采硬件样本回归库
- **现状不足**：当前在 Linux 宿主机上完成了 MinGW-w64 交叉编译验证，但尚未在真实 Windows 10/11 桌面环境执行高分屏（150%/200% DPI）与 `winmm` 声卡实机验收；测试数据主要为合成与兼容生成数据。
- **下一步计划**：
  1. 在 Windows 原生 MSVC + Qt 6 环境完成打包（`tools/package_windows.ps1`）与实机声卡回放验收。
  2. 收集 RK3588 + MEMS 传感器实机录制的振动与声音样本库，纳入自动化回归基线。

