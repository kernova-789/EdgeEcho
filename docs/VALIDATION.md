# EdgeEcho 当前验证状态（0.3.0）

更新日期：2026-10-29，Asia/Shanghai。

本文件对应当前源码；[0.1历史报告](VALIDATION_0.1_2026-10-29.md)及未带current前缀的旧JSON保留历史用途，不能替代本轮结果。

## 统一来源

当前构建身份：`source-sha256:225461afb282f90f81a34589336e986fc2aea459023a24db7fcc79843624c05a`。

[统一验证报告](evidence/current-validation.json)记录新生成的CTest/JUnit、实际命令退出码、源码与二进制SHA256及ABI。源码前后稳定；GUI、CLI、sanitizer与交叉产物绑定同一源身份。验证器拒绝空/跳过/陈旧/不完整结果。

| 配置 | 实际结果 | 范围 |
| --- | --- | --- |
| Linux Release | 13/13入口通过 | 数据库、编码/时钟、源会话、原子导出、XYZ/probe、CLI、验证器、Qt图表/日历和整窗交互 |
| Linux ASan + UBSan | 9/9入口通过 | Debug核心/CLI，开启泄漏检查与UBSan失败退出；不含GUI的sanitizer长期运行 |
| Windows x86-64交叉构建 | 6个PE产物编译/架构/身份检查通过 | CLI及数据库、编码、会话、导出、浏览测试；**未在Windows执行** |

整窗交互37项检查，包括原有播放/分页/源替换、日历精度和时区切换、XYZ、缩放/平移/复位、精确原始样本、缓存失效和尾窗口预取命中。详情在 `build/test-data/gui-inputs/interaction/interaction-result.json`。

## 兼容与源保护

[兼容证据](evidence/current-compatibility.json)使用未修改的原EdgeSense Replay Store，写出3000条模拟标量记录及1个事件。EdgeEcho验证分页、ID、时间、metadata、事件和曲线，读取源哈希不变，无WAL/SHM/journal。参考项目37个构建/源码输入前后指纹一致。

当前只支持关闭写入并完成checkpoint的离线库或一致备份；拒绝非空WAL/journal，使用immutable避免源目录sidecar写入。源身份校验是误用防护，不能证明并发活动库一致性。本轮未修改EdgeEcho目录之外的文件。

## 100次重复查询基准

来源：[current-performance.json](evidence/current-performance.json)。本机AMD Ryzen5 4600U、Linux x86-64，合成库248,623,104字节；单个离线连接反复查询，缓存未受控，所有结果数量逐次核对。

| 工作负载 | 次数 | P50 | P95 | 输出项数 |
| --- | --- | --- | --- | --- |
| 819.2万波形样本 | 100 | 638.768ms | 674.078ms | 3000 |
| 20万标量记录 | 100 | 722.168ms | 760.198ms | 4177 |
| 历史尾部1秒 | 100 | 0.660ms | 1.036ms | 100 |
| 10秒XYZ同屏数据 | 100 | 57.051ms | 64.463ms | 9000 |
| 10秒区间原始样本查询 | 100 | 27.982ms | 31.048ms | 1 |

单进程五组基准峰值RSS为 31,412 KiB（约 30.7 MiB）。数据库哈希前后相同。

上述P95是核心查询延迟，**不包含GUI调度、绘图或进程启动**。未据此验收GUI端到端seek≤250ms、冷缓存、Windows或RK3588性能。

## 交付包

Linux运行包由最终当前GUI/CLI生成，部署Qt及必要非基础运行库、offscreen/xcb/minimal插件、文档、许可和SHA256清单。当前包验收结果见[current-package.json](evidence/current-package.json)，清除开发环境变量并迁移到中文空格目录。它只证明本机ABI上的迁移运行，仍依赖兼容glibc、系统字体与图形驱动，不证明旧版发行版或干净系统镜像。

Windows脚本与原生CI构建、windeployqt打包流程已提供；本机无Windows原生环境，未声明WindowsGUI运行、DPI或独立包实际通过。

## 保留的验收项目

- 已确认来源的真实传感器振动/声音/特征数据；现有兼容与性能数据仍为模拟。
- Windows原生桌面及无开发环境安装包、DPI与文件对话框。
- Linux干净系统/其他发行版/实际图形驱动。
- 百万/千万记录、较多系列与事件的首次打开性能；目录统计仍为全库精确操作。
- 8小时连续运行及100次GUI端到端取消/seek计时；本轮百次查询不等于长期验收。
- 介质故障、强制退出和真实掉电专项；不把fsync及故障注入单测直接当作掉电验收。

实施复盘：[第二阶段](PHASE2_DEVELOPMENT_REPORT_2026-10-29.md)、[第三阶段](PHASE3_DEVELOPMENT_REPORT_2026-10-29.md)。
