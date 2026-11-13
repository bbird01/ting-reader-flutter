# Ting Reader Flutter

Ting Reader Flutter 是听悦的跨平台客户端，用于连接 Ting Reader 服务端，提供书架、书单、下载、本地离线播放和沉浸式播放体验。
此为fork原作者项目：https://github.com/dqsq2e2/ting-reader-flutter，在1.4.5基础上修改了WINDOWS媒体会话（SMTC）和最小化托盘功能。
## 平台能力

| 平台 | 播放 | 后台/系统媒体控制 | 本地下载 | 说明 |
| --- | --- | --- | --- | --- |
| Android | 支持 | 支持通知栏和锁屏媒体控制 | 支持 | 已配置通知权限、前台播放服务和 HTTP 局域网访问。 |
| iOS | 支持 | 支持系统 Now Playing/锁屏控制 | 支持 | 需要正常签名，并启用 Background Modes: Audio。 |
| macOS | 支持 | 支持系统媒体集成 | 支持 | 已启用 audio background 和网络客户端 entitlement。 |
| Windows | 支持 | 支持 SMTC 系统媒体会话 | 支持 | 播放由 `just_audio_windows` 提供；SMTC 经 `smtc_windows`（Rust）桥接；另支持最小化到托盘。 |
| Linux | 支持 | 当前未接入 MPRIS | 支持 | 播放由 `just_audio_media_kit` + `media_kit_libs_linux` 提供。 |
| Web | 支持 | 受浏览器后台策略限制 | 浏览器能力受限 | 适合预览和桌面浏览器使用。 |

## Windows 系统集成

### 系统媒体会话（SMTC）

Windows 端通过 `smtc_windows` 发布系统媒体会话，播放状态、章节元数据（标题/作者/专辑/封面）和时间轴会随播放实时同步，可被系统媒体浮窗、键盘媒体键以及第三方控制器（如灵动岛类工具）识别和控制。

- 已启用控制：播放、暂停、上一章、下一章。
- 未启用控制：停止、快进、快退；系统侧拖动进度（seek）暂不响应。
- 会话仅在 Windows 平台初始化；初始化失败只记录日志，不影响应用内播放。
- 桥接依赖 Rust：`smtc_windows` 为 ffi 插件，构建时由 cargokit 编译原生库；`flutter_rust_bridge` 通过 `dependency_overrides` 锁定为与 codegen 一致的 2.11.1，升级时需同步调整。

### 最小化到托盘

Windows 端提供原生托盘图标（Win32 `Shell_NotifyIcon`），关闭或最小化主窗口时可隐藏到通知区域，播放与系统媒体控制保持可用。

- 行为可选：退出程序 / 最小化到任务栏 / 最小化到托盘，配置入口为「我的 → 个性化设置 → 桌面窗口 → 关闭主窗口时」，随账号设置同步。
- 双击托盘图标还原主窗口；右键菜单提供「显示主界面」和「退出软件」，退出走正常进度上报流程。
- 资源管理器重启后托盘图标自动恢复；托盘图标添加失败时不会隐藏窗口，避免界面丢失。

## 本地开发

```powershell
flutter pub get
flutter analyze
flutter test
flutter run -d windows
```

Android 调试：

```powershell
flutter run -d android
flutter build apk --release
```

Windows 构建除 Flutter 外还需要：

- Visual Studio 2022「使用 C++ 的桌面开发」工作区（含 MSVC 与 Windows SDK）。
- Rust 工具链：`rustup` 默认 stable，目标 `x86_64-pc-windows-msvc`（SMTC 桥接的原生库由 cargokit 在构建时编译）。

```powershell
flutter build windows --release
```

Linux 构建需要系统依赖：

```bash
sudo apt-get update
sudo apt-get install -y ninja-build libgtk-3-dev
flutter build linux --release
```

## 发布产物

GitHub Actions 会在推送 `v*` tag 或手动触发时构建以下产物：

- `ting-reader-flutter-android.apk`
- `ting-reader-flutter-android.aab`
- `TingReader-windows-x64.zip`
- `TingReader-linux-x64.tar.gz`
- `TingReader-macos.zip`
- `TingReader-ios-nosign.zip`

Windows job 会额外安装 Rust stable 工具链（`x86_64-pc-windows-msvc`）以编译 SMTC 原生库。

官网客户端下载接口会读取 `dqsq2e2/ting-reader-flutter` 的最新 Release，并使用这些固定文件名生成下载链接。

## 平台注意事项

- Android 13+ 首次启动会主动请求通知权限，避免通知栏播放器不可用。
- Windows 已接入 SMTC 与托盘；系统侧仅支持播放/暂停/切章，不支持 seek、停止与快进快退。Linux 仍未接入 MPRIS，只保证应用内播放。
- Linux 播放走 media_kit native libs；如果发行版改为系统 libmpv，需要同步调整打包依赖。
- iOS/macOS 的系统媒体能力需要真实设备和签名环境验证。

## 开源协议

本项目采用 [GNU Affero General Public License v3.0](LICENSE) 协议（SPDX：`AGPL-3.0-only`）。
