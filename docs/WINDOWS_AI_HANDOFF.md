# EdgeEcho · Windows 平台 AI 接棒与开发指南（v0.4.0 → v0.5.0）

本文档专为**接手本项目 Windows 平台构建、验证与后续特性开发的 AI 助手**编写。汇总了项目当前架构状态、已完成能力、Windows 平台构建命令与下一步核心任务。

---

## 一、项目背景与当前基线状态（v0.4.0）

- **项目定位**：`EdgeEcho` 是专为工业传感器数据离线回放项目 `EdgeSense Replay`（SQLite Schema v3，`application_id = 1163088464`）打造的跨平台（Linux / Windows）C++17 + Qt 6 桌面回放、听诊与分析软件。
- **源码仓库**：`https://github.com/kernova-789/EdgeEcho` (当前分支 `main`)
- **核心能力已全部就绪（跨系统通用）**：
  1. **数据解码与绘图**：标量、三轴振动 `iio-le-24-v1`（XYZ 同轴同屏）、声音 PCM `iio-le-16-v1`、29 维特征包 `iio-le-72-v1`、通用小端波形；多通道峰值保留防抽点失真，非 `good` 质量自动断线。
  2. **跨平台实时声音播放与听诊**：`AudioPlayer` 在 Linux 动态加载 PulseAudio/ALSA，Windows 端原生链接 `winmm`（`waveOut` 实现于 `src/audio.cpp`）；支持与回放轴同步播放、音量/静音控制、当前窗口试听与电平表。
  3. **频域 FFT 频谱分析**：去直流 + Hann 窗 Cooley-Tukey Radix-2 FFT（`compute_spectrum`），支持一键切换频谱图与主频自动标注。
  4. **原子 CSV / WAV 导出**：`Database::export_csv` 与 `Database::export_wav` 均通过私有临时文件原子提交，绝不覆盖已有文件。
  5. **双系统原生组件风格 (`NativeOsProxyStyle`) 与平滑切换动画**：
     - **Windows 11 风格**：全套 WinUI 3 原生微圆角卡片、底部强调线输入框、同心圆滑块、Mica 云母及暗色模式窗口属性（通过 `dwmapi.dll`）。
     - **Linux Ubuntu 风格**：Ubuntu GTK4 / Libadwaita / Yaru 原生组件与官方图标库。
     - **主题模式**：支持跟随系统（检测注册表 `AppsUseLightTheme`）、固定浅色、固定深色，并在切换时触发 380 ms 径向波纹与 60 FPS 色彩插值。

---

## 二、Windows 平台 AI 首要任务：构建与打包完整桌面 GUI 发行包

### 1. 现状与问题说明
在当前的 Release v0.4.0 中，用户下载 `edgeecho_cli-0.4.0-windows-x86_64.exe` 双击后黑框一闪而过，原因是该文件是**仅供终端调用的无界面命令行工具（CLI，4.3 MB）**。
**首要任务**是在 Windows 真实环境下编译并打包出**包含图形界面 `edgeecho.exe` 与全部 Qt6 运行时库的 `EdgeEcho-0.4.0-windows-x86_64.zip`（约 35~40 MB）**，并将其补充发布到 GitHub Release 中。

### 2. 本地已预先做好的 Windows 兼容性修复
我们已经在 `main` 分支代码中加入了 Windows 专用的底层适配：
1. **`CMakeLists.txt`**：已配置 `if(WIN32) target_link_libraries(edgeecho PRIVATE advapi32 dwmapi) endif()`，确保读取深浅色注册表与 `dwmapi` 调用顺利链接。
2. **`app/main.cpp`**：已加入 `initWindowsConsoleOutput()`（基于 `AttachConsole` / `_open_osfhandle`），确保 GUI 程序在命令行或测试管道中执行 `--version`、`--smoke` 时标准输出（stdout）不会被 Windows GUI 子系统丢弃。

### 3. Windows 平台原生构建与测试步骤（PowerShell）

请在装有 **Visual Studio 2022 (MSVC C++)** 与 **Qt 6 (建议 6.5 ~ 6.8，msvc2022_64)** 的 Windows 环境中打开「适用于 VS 2022 的 x64 本机工具命令提示」或开发者 PowerShell：

```powershell
# 1. 克隆或拉取最新代码
git clone https://github.com/kernova-789/EdgeEcho.git
cd EdgeEcho

# 2. CMake 配置（替换为你机器上的 Qt6 安装路径）
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.8.2/msvc2022_64" -DCMAKE_BUILD_TYPE=Release

# 3. 编译核心、CLI、GUI 及单元测试
cmake --build build --config Release --parallel

# 4. 执行全量自动化测试（含 GUI 离线冒烟与交互测试）
ctest --test-dir build -C Release --output-on-failure

# 5. 执行官方标准 Windows 打包脚本（自动调用 windeployqt 收集全部依赖 DLL 与插件）
powershell -ExecutionPolicy Bypass -File .\tools\package_windows.ps1 -BuildDir build -Output dist

# 6. 验证打包产物完整性与离线独立运行能力
python tools\validate_package.py --package dist\EdgeEcho-0.4.0-windows-x86_64 --validation-report docs\evidence\current-validation.json
```

打包成功后，将在 `dist/` 目录下生成：
- **`dist/EdgeEcho-0.4.0-windows-x86_64.zip`**
解压后直接双击 `bin\edgeecho.exe`，应直接展示出 Windows 11 Fluent 风格的桌面主窗口，且无多余控制台黑框弹出。

### 4. 补充上传至 GitHub Release
将生成的 `EdgeEcho-0.4.0-windows-x86_64.zip` 上传至 GitHub 仓库：
`https://github.com/kernova-789/EdgeEcho/releases/tag/v0.4.0`

---

## 三、Windows 实机交互与视觉验收清单

在 Windows 10/11 实机上运行时，请重点检验以下细节：
1. **Windows 11 原生特效**：
   - 窗口圆角与标题栏：在 Win11 下是否正确应用 `DWMWA_WINDOW_CORNER_PREFERENCE` 圆角和暗色标题栏。
   - 系统主题跟随：在 Windows 设置中将系统切换为“深色”/“浅色”，软件在“跟随系统”模式下应在 1~2 秒内自动平滑淡入新主题。
2. **音频听诊**：
   - 打开测试数据库（`build/test-data/demo.db`），选中声音通道（`demo-sound`），点击播放或「试听当前窗口」，验证 Windows 声卡是否有正常声音输出，电平表是否同步跳动。
3. **高分屏（HiDPI）**：
   - 在 125%、150% 或 200% 缩放下，Win11 微圆角、滑块、图标和波形是否清晰无虚焦。

---

## 四、后续版本（v0.5.0+）开发路线图

Windows 桌面包交付并验收完毕后，即可根据用户需求开展下一步开发：

1. **优先级 1：物理工程量纲校准**
   - 振动与声音当前保存为 ADC 原始整型 `count`，需读取 `metadata` 中的灵敏度/增益参数，支持界面一键切换为物理单位（$\text{m/s}^2$、$g$、$\text{Pa}$、$\text{dB SPL}$）。
   - 为 `iio-le-72-v1` 的 29 个特征通道提供完整的中文工程语义标签。
2. **优先级 2：多子图联动（Linked Subplots）与 STFT 时频瀑布图**
   - 支持上下排列 2~4 个子图（共享时间轴与播放光标），同时对比振动、声音及 STFT 频谱瀑布图（Spectrogram 热力图）。
3. **优先级 3：音频变速不变调（WSOLA）与带通滤波听诊**
   - 改变回放倍速时保持音频音调不变；支持在频谱图上框选滤波频段，实时听诊轴承/齿轮故障特征频。
