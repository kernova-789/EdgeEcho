# EdgeEcho v0.5.0 · Windows 平台 AI 编译、打包与 GitHub Release 上传流程

> **致 Windows 平台的 AI 助手**：
> 1. **禁止修改功能代码**：`EdgeEcho v0.5.0` 的全部核心与桌面端功能（`mems_driver` 29 通道物理工程量纲校准、STFT 时频瀑布图、时频联动双子图、WSOLA 倍速保调音频、零相位 Butterworth 数字带通滤波听诊、Windows 11 Fluent / WinUI 3 官方组件风格与平滑波纹主题动画）**已在 Linux 主开发环境全部开发并验证完毕**。
> 2. **源码指纹保护机制**：本项目内置严格的源码指纹校验（`tools/validation_support.py` 会计算 `CMakeLists.txt`、`include/`、`src/`、`app/`、`tools/`、`tests/` 的 SHA-256 指纹并嵌入可执行文件）。**请勿修改上述目录下的任何源码文件**，否则会导致构建产物与验证报告的 `source-sha256` 不一致。
> 3. **你的唯一核心任务**：在 Windows 环境中关联本机的 **MSVC 2022 + Qt 6 (msvc2022_64)** 图形渲染引擎，编译出带完整图形界面的 `edgeecho.exe`，使用 `tools/package_windows.ps1` 打包生成 **`dist/EdgeEcho-0.5.0-windows-x86_64.zip`**，并将其上传到 GitHub 仓库 `kernova-789/EdgeEcho` 的 **`v0.5.0` Release** 中。

---

## 第一步：定位本机 Visual Studio 2022 (MSVC) 与 Qt 6 路径

在 Windows PowerShell 中执行以下探测脚本，自动定位本机安装的 Qt 6 (`msvc*_64`) 与 Visual Studio CMake/MSVC 环境：

```powershell
# 1. 自动搜索常见位置的 Qt 6 (msvc*_64) 目录
$QtCandidates = @(
    "C:\Qt", "D:\Qt", "$env:USERPROFILE\Qt", "$env:SystemDrive\Qt"
) | Where-Object { Test-Path $_ } | ForEach-Object {
    Get-ChildItem -Path $_ -Directory -Depth 2 -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "msvc*64*" -and (Test-Path (Join-Path $_.FullName "bin\windeployqt.exe")) }
} | Sort-Object FullName -Descending

if ($QtCandidates.Count -eq 0) {
    Write-Error "未在默认路径找到 Qt 6 (msvc*_64)，请手动将 `$QtPrefix` 设置为包含 bin/windeployqt.exe 的目录"
} else {
    $QtPrefix = $QtCandidates[0].FullName
    Write-Host "已定位 Qt 6 目录: $QtPrefix" -ForegroundColor Green
}

# 2. 将 Qt6 bin 目录加入当前会话 PATH（关键：package_windows.ps1 依赖 windeployqt.exe 在 PATH 中）
$env:PATH = "$QtPrefix\bin;$env:PATH"
Get-Command windeployqt.exe
```

---

## 第二步：配置、编译与全量自动化测试（Release 模式）

> **注意**：请确保在已加载 MSVC x64 环境变量的终端（如 `Developer PowerShell for VS 2022`）中运行，或通过 `VsDevCmd.bat -arch=x64` 初始化环境。

在项目根目录 `EdgeEcho` 下依次执行：

```powershell
# 1. 清理旧的构建缓存（避免残留 Linux 构建产物冲突）
if (Test-Path build) { Remove-Item build -Recurse -Force }
if (Test-Path dist)  { Remove-Item dist  -Recurse -Force }

# 2. CMake 配置（显式开启 GUI 构建并指定 Qt 6 路径）
cmake -S . -B build -DEDGEECHO_BUILD_GUI=ON -DCMAKE_PREFIX_PATH="$QtPrefix" -DCMAKE_BUILD_TYPE=Release

# 3. 编译核心库、命令行工具 (edgeecho_cli.exe)、图形界面主程序 (edgeecho.exe) 与全部测试
cmake --build build --config Release --parallel 4

# 4. 运行 CTest 全部 13 项自动化测试（含 GUI 离线渲染、交互回归、音频/FFT/STFT/量纲标定测试）
ctest --test-dir build -C Release --output-on-failure
```

---

## 第三步：生成新鲜验证证据与打包 Windows 独立桌面运行包

项目的打包验证脚本要求提供当前平台最新生成的 `current-validation.json`。请按顺序执行以下三条命令：

```powershell
# 1. 运行官方验证器，生成匹配当前源码 SHA-256 的验证报告
python tools/validate_current.py --profiles release --parallel 4 --report docs/evidence/current-validation.json

# 2. 运行官方 Windows 打包脚本（自动调用 windeployqt 收集所有 Qt6 GUI/Widgets 渲染引擎 DLL、平台插件与编译器运行时）
powershell -ExecutionPolicy Bypass -File .\tools\package_windows.ps1 -BuildDir build -Output dist -Configuration Release

# 3. 验证生成的独立运行包（自动在含中文与空格的隔离目录中测试 edgeecho.exe 与 edgeecho_cli.exe）
python tools/validate_package.py --package dist/EdgeEcho-0.5.0-windows-x86_64 --validation-report docs/evidence/current-validation.json
```

成功后，你将得到完整的 Windows 桌面版压缩包：
- **文件路径**：`dist\EdgeEcho-0.5.0-windows-x86_64.zip`
- **包内结构**：
  - `bin\edgeecho.exe`：**带 Windows 11 Fluent 图形界面的桌面端主程序**（已配置 `WIN32_EXECUTABLE TRUE`，双击直接打开 GUI 窗口，绝不会出现黑框闪退）。
  - `bin\edgeecho_cli.exe`：命令行工具。
  - `bin\Qt6Core.dll`、`bin\Qt6Gui.dll`、`bin\Qt6Widgets.dll`、`bin\platforms\qwindows.dll`、`bin\styles\qmodernwindowsstyle.dll` 等完整 Qt 图形引擎运行时依赖。

---

## 第四步：上传至 GitHub 仓库与 Release `v0.5.0`

需要将构建出的 **`dist\EdgeEcho-0.5.0-windows-x86_64.zip`** 上传至 GitHub 仓库 `https://github.com/kernova-789/EdgeEcho` 的 `v0.5.0` Release 页面（若代码有未推送提交，也请一并 `git push origin main --tags`）。

### 方式 A：如果 Windows 机器已安装并登录 GitHub CLI (`gh`)

```powershell
# 若 Release v0.5.0 已存在，直接补充上传 Windows 桌面版压缩包（--clobber 允许覆盖同名资产）
gh release upload v0.5.0 dist\EdgeEcho-0.5.0-windows-x86_64.zip --repo kernova-789/EdgeEcho --clobber
```

*(注：若远端尚未创建 `v0.5.0` Release，可执行：`gh release create v0.5.0 dist\EdgeEcho-0.5.0-windows-x86_64.zip --repo kernova-789/EdgeEcho --title "EdgeEcho v0.5.0" --generate-notes`)*

### 方式 B：如果未安装 `gh`，直接使用 PowerShell 调用 GitHub REST API 上传

无需安装任何额外软件，只需设置 GitHub Personal Access Token (`$env:GITHUB_TOKEN`) 并运行以下原生 PowerShell 脚本即可完成上传：

```powershell
$Token = $env:GITHUB_TOKEN  # 请替换为具有 repo 权限的 GitHub Token
$Repo  = "kernova-789/EdgeEcho"
$Tag   = "v0.5.0"
$ZipPath = Resolve-Path "dist\EdgeEcho-0.5.0-windows-x86_64.zip"
$AssetName = "EdgeEcho-0.5.0-windows-x86_64.zip"

$Headers = @{
    "Authorization" = "Bearer $Token"
    "Accept"        = "application/vnd.github+json"
    "X-GitHub-Api-Version" = "2022-11-28"
}

# 1. 查询或创建 v0.5.0 Release
try {
    $Release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/tags/$Tag" -Headers $Headers -Method Get
} catch {
    $Body = @{ tag_name = $Tag; name = "EdgeEcho v0.5.0"; draft = $false; prerelease = $false } | ConvertTo-Json
    $Release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases" -Headers $Headers -Method Post -Body $Body -ContentType "application/json"
}

# 2. 若已存在同名旧资产则先删除
foreach ($Asset in $Release.assets) {
    if ($Asset.name -eq $AssetName) {
        Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/assets/$($Asset.id)" -Headers $Headers -Method Delete
    }
}

# 3. 上传 EdgeEcho-0.5.0-windows-x86_64.zip
$UploadUrl = ($Release.upload_url -replace '\{\?name,label\}', '') + "?name=$AssetName"
$Uploaded = Invoke-RestMethod -Uri $UploadUrl -Headers $Headers -Method Post -InFile $ZipPath -ContentType "application/zip"
Write-Host "上传成功！下载链接: $($Uploaded.browser_download_url)" -ForegroundColor Green
```
