# PadDrawBoard v0.1 交接文档

## 1. 目标

- 在 Windows 与 Android 之间建立可用的 PadDrawBoard 实时板书/输入链路。
- 通过 USB ADB 三通道传输控制、视频与输入数据；遥测复用控制通道。
- 在 Windows 端使用 DXGI、D3D11 与 Media Foundation（MF H.264）完成采集/编码，在 Android 端使用 MediaCodec 完成解码/呈现。
- 支持 Windows Ink 输入架构，并保留后续笔按钮、悬停和三键发布能力的扩展空间。
- 提供可构建、可测试、可打包的 v0.1 工程基线。

## 2. 非目标

- v0.1 不承诺 penButtons、hover 或三键发布门槛已完成。
- v0.1 不承诺 Blender 全套流程已完成。
- v0.1 不承诺 1 小时 soak 测试已完成。
- 本文不将未完成项描述为已通过，也不扩展未验证的性能或兼容性结论。

## 3. 仓库地图

- `desktop`：Windows 端应用、采集/编码、输入注入与传输实现。
- `android`：Android 端应用、MediaCodec 解码/呈现及触控笔采集实现。
- `protocol`：Windows 与 Android 共用的协议、消息和通道定义。
- `tools`：构建、测试、打包、诊断或辅助工具。

## 4. 核心架构

### USB ADB 三通道

桌面端只监听 `127.0.0.1:48100/48101/48102`，通过 ADB reverse 建立控制、视频和输入三条 TCP 通道。遥测、时钟同步和请求 IDR 等消息复用控制通道。

### Windows 端：DXGI + D3D11 + MF H.264

Windows 端以 DXGI/D3D11 获取和处理图像，再交由 Media Foundation 的 H.264 编码链路生成视频数据，通过 USB ADB 视频通道发送至 Android。

### Android 端：MediaCodec

Android 端使用 MediaCodec 接收视频通道中的 H.264 数据，完成解码并输出到呈现链路。

### Windows Ink

Android 采集笔迹、压力、倾斜、距离和触控样本，经输入通道发送到桌面端；桌面端使用 `CreateSyntheticPointerDevice` / `InjectSyntheticPointerInput` 注入 Windows Ink。笔按钮与悬停能力仍属于未完成门槛。

## 5. 开发环境

- Windows 11 x64
- Visual Studio 2022
- MSVC 143
- Windows 11 SDK
- Android API 36
- Android NDK 28
- CMake 3.22
- JDK
- Android Platform Tools 36.0.2

## 6. 构建、测试与打包命令

### Windows 构建与 CTest

当前已验证的开发构建使用 Android SDK 附带的 CMake 3.22.1 和 MinGW：

```powershell
$env:PATH = "C:\Users\<用户名>\AppData\Local\Android\Sdk\cmake\3.22.1\bin;D:\mingw-w64\bin;$env:PATH"
cmake -S . -B out\connection-fix -G "MinGW Makefiles" -DPDB_BUILD_TESTS=ON
cmake --build out\connection-fix --parallel
ctest --test-dir out\connection-fix --output-on-failure
```

### Android Gradle

```powershell
Set-Location android
.\gradlew.bat assembleDebug
.\gradlew.bat test
```

### Windows 打包

```powershell
$env:PATH = "C:\Users\<用户名>\AppData\Local\Android\Sdk\cmake\3.22.1\bin;D:\mingw-w64\bin;C:\Program Files (x86)\NSIS;$env:PATH"
.\packaging\build-windows-packages.ps1 `
  -SourceDirectory (Get-Location).Path `
  -BuildDirectory (Join-Path (Get-Location).Path 'out\release-current') `
  -OutputDirectory (Join-Path (Get-Location).Path 'artifacts\release-current\windows') `
  -Configuration Release `
  -Generator 'MinGW Makefiles'
```

## 7. 实机 Pad7 结果

- 首 IDR：2.24s
- 三通道：3.18s
- 视频遥测：3.87ms
- RTT：6.9ms
- 丢帧：0
- CTest：7/7

以上为已记录结果；未将其外推为其他设备、网络条件或更长时间运行下的保证。

## 8. 关键修复清单

- 单实例互斥和 `SO_EXCLUSIVEADDRUSE`，避免多个桌面实例争抢同一端口。
- 修复 ADB 会话 token 启动竞态，并以 `am start -S` 确保 Android 使用新 token。
- 限制 ADB 子进程句柄继承，防止桌面退出后 ADB 继续占用三条监听 socket。
- 修复 Media Foundation H.264 的输出/输入类型设置顺序和硬件编码器兼容性。
- 为 BGRA 捕获纹理补齐 D3D11 视频处理器要求的绑定标志。
- 视频发送连续背压超过 2 秒时结束会话并重连，避免假 `CONNECTED`。
- 同时依据 `CleanPoint` 和 Annex-B NAL type 5 识别 IDR。
- 按接口要求以 `VT_UI4/1` 设置 ForceKeyFrame，保证静止桌面也能从首帧解码。
- 设备选择时忽略标准 `emulator-*`，但仍拒绝多台物理设备。

## 9. 配置与遥测

- 配置文件：`%APPDATA%\PadDrawBoard\config.json`
- 遥测文件：`%APPDATA%\PadDrawBoard\telemetry.jsonl`

## 10. 故障排查

### 状态为 CONNECTED 但黑屏

- 确认视频通道已建立且仍有数据流入。
- 检查 Windows DXGI/D3D11 获取、MF H.264 编码和 Android MediaCodec 解码链路。
- 查看 `%APPDATA%\PadDrawBoard\telemetry.jsonl`，对照首 IDR 与视频遥测记录定位卡点。
- 重新连接设备并确认 ADB 三通道均已恢复。

### 状态为 RECONNECTING

- 检查 USB 连接、设备授权和 Platform Tools/ADB 状态。
- 确认三个 ADB 通道是否都能重新建立。
- 若反复重连，保留遥测文件并记录发生时间、设备状态及 ADB 输出后再定位。

### 启动慢

- 重点检查首 IDR 生成、视频编码器初始化和三通道建立耗时。
- 对照已记录的 Pad7 基线：首 IDR 2.24s、三通道 3.18s。
- 避免将一次启动耗时直接认定为普遍性能结论。

### 端口占用

- 查找占用相关本地端口的进程并停止冲突实例。
- 确认没有遗留的 PadDrawBoard 或 ADB 转发进程。
- 重新启动 ADB/应用后再次建立三通道；不要在未确认占用者时盲目终止系统进程。

## 11. 未完成门槛

- `penButtons=[]`
- `hover=false`
- 三键发布阻塞
- Blender 全套未完成
- 1 小时 soak 未完成

这些项目仍是发布前门槛，不得标记为已通过。

## 12. 维护优先级

1. 先解决三键发布阻塞及 `penButtons=[]`、`hover=false` 门槛。
2. 完成并验证 Blender 全套流程。
3. 完成 1 小时 soak 测试并记录结果。
4. 保持 USB ADB 三通道、视频首 IDR、重连和启动耗时的回归验证。
5. 维护 CTest、Android Gradle 构建与 Windows 打包链路。
6. 继续完善配置、遥测和故障诊断信息。

## 13. Windows 最终产物校验

目录：`artifacts\release-current\windows`

- EXE SHA256：`638863fa5389b5cf474646831bfc18d270687a0dce9ddc420a48b8bec944419d`
- ZIP SHA256：`c671dc08b99cdee8be07255ff82c9d76742b1e66c22067b7e9441fea2f337308`
