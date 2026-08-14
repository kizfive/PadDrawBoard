# PadDrawBoard 当前交接（2026-08-15）

> 本节是当前事实来源。下方“历史交接”保留早期基线记录，其中部分状态已被本轮实机修复取代。

## 当前结论

- Windows 与 Android 客户端已经在一台 Xiaomi Pad 7 类设备上完成实机联调。
- 控制、视频和输入三个 ADB reverse 通道均已建立并保持 `Established`；端口仍为 48100、48101、48102。
- 笔输入、手指拖动/点击、屏幕旋转后的坐标映射、视频显示和熄屏恢复均完成针对性修复。
- Windows 托盘界面已精简并中文化；Windows 与 Android 均已接入新的 PadDrawBoard 应用图标。
- 本轮最后验证：Windows CTest 7/7 通过，Android `testDebugUnitTest` 与 `assembleDebug` 通过，Debug APK 已覆盖安装并成功启动。
- 接手会话复验（2026-08-15 凌晨）：Windows（MinGW）构建通过、CTest 7/7；Android `testDebugUnitTest` 与 `assembleDebug` 通过。
- 接手会话还发现一个**未解决**的严重问题：在一台 AMD 核显 + NVIDIA 独显混合显卡的机器上，视频画面在会话建立后约 15-25 秒永久冻结（触控与控制通道不受影响、无会话断开）；同一份最新代码在纯 Intel 核显机器上实时刷新正常。完整排查记录见下文专节。
- 本机测试时曾同时运行 MuMu 模拟器远程服务、Wallpaper Engine 与 NVIDIA App 浮窗等屏幕相关组件，已全部停用，但冻结未解除。

架构、构建前置条件和完整验收方法不在此重复，分别参见：

- `docs/architecture.md`
- `README.md`
- `docs/testing.md`

## 本轮实现范围

### 输入与触控

- Android 端按一次手势的首个落点决定是否由绘图区捕获，后续 MOVE/UP 不会因为滑入状态栏而丢失。
- MotionEvent 历史样本按 MOVE 发送，多指 POINTER_DOWN/POINTER_UP 会给每个 pointer 生成正确动作。
- Android 发送输入和控制消息改为各自的单线程执行器，避免在 UI 线程同步写 socket 导致触摸或笔事件触发崩溃。
- Windows 按时间戳和 pointer ID 分组注入历史触控样本，避免同一批次出现重复 pointer ID。
- Android View 已经提供旋转后的坐标，Windows 端不再重复旋转；横竖屏位置映射由此恢复正常。
- 防误触只在笔尖实际按下/移动时抑制手指。笔抬起后，手指仍可用于 Windows 触摸拖动和点击，但不会冒充笔迹。

### 会话与视频

- 新增客户端认证握手看门狗：ADB 启动成功但认证 socket 在超时内未建立时，会刷新一次性 token 并重新拉起 Android Activity，修复熄屏恢复后的 `invalid or missing session authentication token`。
- Media Foundation H.264 输出处理现在覆盖 FORMAT_CHANGE、输出类型重新协商、异步 MFT 事件和空输出重试，降低编码器重启及黑屏概率。
- 相关决策逻辑均补充原生单元测试；不要在没有硬件复测的情况下删去 stream-change 分支。

### Windows 界面与品牌资源

- 托盘状态、连接信息、显示器、码率、防误触和诊断工具已中文化；仅保留当前确实有功能实现的选项。
- Logo 源文件位于 `assets/branding/`。
- `tools/generate_app_icons.py` 可从透明 Logo 重新生成 Windows ICO、Android 普通/圆形/自适应图标。
- Windows 资源脚本会把图标嵌入 EXE，托盘和窗口类也加载同一资源；Android Manifest 使用 `@mipmap/ic_launcher`。

## 当前构建产物与实机状态

- Windows 构建输出：桌面端目标 `PadDrawBoard.exe`，位于本地构建目录（`out\*` 或 `build\`，随各机惯例而定）。
- Android Debug APK：`android/app/build/outputs/apk/debug/app-debug.apk`（标准 Gradle 输出路径，各机一致；2026-08-14 已重新构建验证）。
- `build/`、`out/` 与 `artifacts/` 是本地生成目录，不应加入 Git；打包与便携测试产物均位于 `artifacts/` 下。

最近一次部署后，Windows 和 Android 进程均正常运行，三个通道均为已连接状态。设备序列号和任何本机账户信息不要写入仓库或日志样例。

**注意**：工作区中当前有 6 个文件未提交的排查/修复改动（见下方排查记录），下次会话先确认其状态再构建。

## 视频冻结排查记录（2026-08-15，未解决）

> 本机指 AMD 核显 + NVIDIA 独显混合显卡的 Windows 机器；参考机为纯 Intel 核显机器（最新代码在该机上实时刷新与触控均正常）。

### 现象

- 会话建立成功、三通道 `Established`、触控与笔输入正常、客户端能解码并显示启动阶段的首批帧。
- 会话建立后约 15-25 秒，视频画面永久冻结：桌面端不再采集、不再编码，客户端 `video_latency_us` 归零；控制/输入通道与客户端遥测完全不受影响。
- 多次运行（Debug 与 Release、MinGW）均可复现；分支提交之前的旧构建同样复现，因此**不是本分支引入的回归**。

### 已确认的事实

- 桌面端视频循环冻结时进程 CPU 接近 0（阻塞等待，不是忙循环）；控制线程仍在每秒写遥测。
- 循环心跳探针（`video_loop_probe` 遥测）显示视频循环在启动后第 50~190 次迭代之间停摆，冻结点漂移（竞态特征）。
- 编码器阶段日志（`%APPDATA%\PadDrawBoard\encoder_stages.log`）显示最后一条记录停在异步编码路径的 `Poll end` 之后、下一轮循环之前。
- 编码器为异步的 NVIDIA H.264 Encoder MFT（NVENC）；`nvidia-smi` 确认 PadDrawBoard 进程运行在独立显卡上。
- 已排除：MuMu 远程服务（已停用并禁用）、Wallpaper Engine（已退出）、NVIDIA App 浮窗（已退出）、旧版代码回归、同步 MFT 重协商死锁假设。
- 连带问题：本机 adb 服务器曾多次整体挂死（`adb shell` 与 `kill-server` 均超时），强杀进程并重启服务器可恢复；与视频冻结是否同源未确认。

### 工作区中未提交的代码改动（6 个文件，均为排查遗留）

- `desktop/src/app/desktop_server.cpp`：`EndSession` 在会话未激活时也写 `pre-active:` 遥测（建议保留）；`video_loop_probe` 心跳遥测（临时，可删）。
- `desktop/src/video/h264_encoder.cpp`：同步路径 stream-change 后不再立即重入阻塞式 `ProcessOutput`（防御性修复，建议保留）；异步路径 stream-change 改为整体重建编码器实例、新增 `PerformFormalStreamReset`（针对 NVENC 中途 `SetOutputType` 挂死的修复，**未能解除冻结**）；临时 `EncoderStageLog` 阶段日志（删除前先保留可复现）。
- `desktop/include/pdb/video/telemetry.h`、`desktop/include/pdb/app/desktop_server.h`、`desktop/src/video/video_pipeline.cpp`：`OnEncoderSelected` 遥测接口（保留有助于诊断，可斟酌）。
- 另存在分支提交前的对照构建工作树（用于差分测试），可复现生成，无需保留。

### 下一步排查建议（按优先级）

1. 对冻结中的进程做线程栈转储（procdump/WinDbg），确认阻塞调用点；现有证据已把它缩小到异步编码调用尾部到下一轮循环之间。
2. 停用 SuperDisplay 服务后复测（本机另一屏幕捕获类组件，尚未排除）。
3. 对照实验：在 NVIDIA 控制面板把 PadDrawBoard 强制到核显运行，验证"独显 NVENC + 桌面复制"是否就是冲突源；若核显可用，可考虑软件编码回退方案。
4. 单独排查 adb 服务器挂死（本机同时存在多个 adb 版本，建议统一使用 36.0.2）。
5. 结案后清理全部临时诊断代码并补原生测试，再考虑推送分支。

## 建议后续验证

1. 在 Blender 中分别验证笔压、连续笔划、单指点击/拖动和多指手势，确认应用级行为与 Windows 原生触摸注入一致。
2. 完成 `docs/testing.md` 中的一小时稳定性测试；当前只完成了短时实机回归。
3. 在另一台不同 DPI/方向的 Android 平板上复测坐标、状态栏手势边界和自适应图标裁切。
4. 继续核实 Xiaomi Focus Pen 的按键与悬停硬件事件；未观测到的能力不得标记为支持。
5. 推送后观察 GitHub Actions；若 Windows 编码器测试在其他驱动环境失败，优先保留并分析失败 stage 与 HRESULT。
6. 若本地 SDK/工具链位置发生过迁移，用新位置完整跑一次 Windows 打包脚本，确认 cmake 与 platform-tools 引用正常。
7. 重启 adb 并复测三通道，确认控制、视频、输入通道恢复正常。
8. 按"视频冻结排查记录"继续定位混合显卡机器的视频冻结；修复前不要在该机器上宣称视频可用。
9. 混合显卡机器结案后，在纯 Intel 核显参考机上复跑一轮验证，确认修复没有破坏原有环境。
10. 处理工作区中未提交的排查改动：保留防御性修复、删除临时日志，补测试后提交。

## 建议技能

- `github:github`：查看仓库、PR 和 issue 状态。
- `github:gh-fix-ci`：处理推送后的 GitHub Actions 失败。
- `computer-use:computer-use`：需要复核 Windows 托盘或其他原生 UI 时使用。
- `agent-reach`：需要查询设备、驱动或平台官方资料时使用；只用于读取互联网内容。
- `handoff`：下一次会话结束前再次压缩更新本交接。

## 历史交接（早期 v0.1 基线）

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
