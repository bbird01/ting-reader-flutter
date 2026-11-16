#include "flutter_window.h"

#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propvarutil.h>

#include <optional>
#include <flutter/standard_method_codec.h>

#include "flutter/generated_plugin_registrant.h"
#include "utils.h"
#include "window_tasks.h"

namespace {

// PKEY_Device_FriendlyName / PKEY_AudioEndpoint_FormFactor 只在本模块使用，
// SDK 未提供导入库定义，这里直接给出常量避免额外链接依赖。
const PROPERTYKEY kDeviceFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14};
const PROPERTYKEY kAudioEndpointFormFactorKey = {
    {0x1da5d803, 0xd492, 0x4edd,
     {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}},
    0};
const PROPERTYKEY kDeviceEnumeratorNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd,
     {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    24};

// 返回当前默认播放设备的端点信息，用于跟踪音频输出路由。
flutter::EncodableMap DefaultRenderOutputInfo() {
  flutter::EncodableMap info;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return info;
  }
  IMMDevice* device = nullptr;
  if (SUCCEEDED(
          enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
      device != nullptr) {
    std::string id;
    LPWSTR wide_id = nullptr;
    if (SUCCEEDED(device->GetId(&wide_id)) && wide_id != nullptr) {
      id = Utf8FromUtf16(wide_id);
      CoTaskMemFree(wide_id);
    }
    std::string name;
    std::string device_enumerator;
    int form_factor = 0;
    IPropertyStore* properties = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
        properties != nullptr) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(
              properties->GetValue(kDeviceFriendlyNameKey, &value)) &&
          value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = Utf8FromUtf16(value.pwszVal);
      }
      PropVariantClear(&value);
      PropVariantInit(&value);
      if (SUCCEEDED(
              properties->GetValue(kAudioEndpointFormFactorKey, &value)) &&
          value.vt == VT_UI4) {
        form_factor = static_cast<int>(value.uintVal);
      }
      PropVariantClear(&value);
      PropVariantInit(&value);
      if (SUCCEEDED(
              properties->GetValue(kDeviceEnumeratorNameKey, &value)) &&
          value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        device_enumerator = Utf8FromUtf16(value.pwszVal);
      }
      PropVariantClear(&value);
      properties->Release();
    }
    info[flutter::EncodableValue("id")] = flutter::EncodableValue(id);
    info[flutter::EncodableValue("name")] = flutter::EncodableValue(name);
    info[flutter::EncodableValue("formFactor")] =
        flutter::EncodableValue(form_factor);
    info[flutter::EncodableValue("enumerator")] =
        flutter::EncodableValue(device_enumerator);
    device->Release();
  }
  enumerator->Release();
  return info;
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());
  const HWND window = GetHandle();
  // 标题栏由 Flutter 绘制，保留系统边框和任务栏菜单。
  SetWindowLongPtr(window, GWL_STYLE, GetWindowLongPtr(window, GWL_STYLE) & ~WS_CAPTION);
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  const HMENU system_menu = GetSystemMenu(window, FALSE);
  AppendMenu(system_menu, MF_SEPARATOR, 0, nullptr);
  AppendMenu(system_menu, MF_STRING, kExitSystemMenuCommand, L"退出软件");

  window_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "ting_reader/window",
      &flutter::StandardMethodCodec::GetInstance());
  window_channel_->SetMethodCallHandler(
      [this](const auto& call, auto result) {
        const auto& method = call.method_name();
        if (method == "setMiniMode") {
          const auto* value = std::get_if<bool>(call.arguments());
          if (!value) { result->Error("arguments", "小窗参数无效"); return; }
          SetMiniMode(*value);
        } else if (method == "setCloseBehavior") {
          const auto* value = std::get_if<std::string>(call.arguments());
          if (!value) { result->Error("arguments", "关闭行为无效"); return; }
          close_behavior_ = *value;
        } else if (method == "minimize") {
          if (close_behavior_ == "tray") {
            HideToTray();
          } else {
            ShowWindow(GetHandle(), SW_MINIMIZE);
          }
        } else if (method == "close") {
          PostMessage(GetHandle(), WM_CLOSE, 0, 0);
        } else if (method == "startDragging") {
          ReleaseCapture();
          SendMessage(GetHandle(), WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (method == "toggleMaximize") {
          const auto* force = std::get_if<bool>(call.arguments());
          const bool maximize = mini_mode_ || (force && *force) || !IsZoomed(GetHandle());
          if (mini_mode_) SetMiniMode(false);
          ShowWindow(GetHandle(), maximize ? SW_MAXIMIZE : SW_RESTORE);
          result->Success(flutter::EncodableValue(IsZoomed(GetHandle()) != FALSE));
          return;
        } else if (method == "defaultAudioOutput") {
          result->Success(
              flutter::EncodableValue(DefaultRenderOutputInfo()));
          return;
        } else if (method == "exit") {
          close_behavior_ = "exit";
          PostMessage(GetHandle(), WM_CLOSE, 0, 0);
        } else {
          result->NotImplemented();
          return;
        }
        result->Success();
      });

  tray_ = std::make_unique<TrayIcon>(
      window, [this]() { RestoreFromTray(); },
      [this]() {
        // 交给 Dart 的 requestExit()，保留退出前的进度上报。
        if (window_channel_) {
          window_channel_->InvokeMethod("exitRequested", nullptr);
        }
      });

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  tray_.reset();
  window_channel_.reset();
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  if (tray_ && tray_->HandleMessage(message, lparam)) {
    return 0;
  }
  if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_MINIMIZE &&
      close_behavior_ == "tray") {
    HideToTray();
    return 0;
  }
  if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_MAXIMIZE && mini_mode_) {
    SetMiniMode(false);
  }
  if (message == WM_SIZE && wparam == SIZE_MAXIMIZED && mini_mode_) {
    SetMiniMode(false);
    ShowWindow(hwnd, SW_MAXIMIZE);
  }
  if (message == kExitApplicationMessage ||
      (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == kExitSystemMenuCommand)) {
    close_behavior_ = "exit";
    PostMessage(hwnd, WM_CLOSE, 0, 0);
    return 0;
  }
  if (message == WM_SIZE && window_channel_ && wparam != SIZE_MINIMIZED) {
    window_channel_->InvokeMethod("maximizedChanged",
        std::make_unique<flutter::EncodableValue>(wparam == SIZE_MAXIMIZED));
  }
  if (message == WM_GETMINMAXINFO && mini_mode_) {
    auto limits = reinterpret_cast<MINMAXINFO*>(lparam);
    const auto dpi = GetDpiForWindow(hwnd);
    limits->ptMinTrackSize.x = MulDiv(420, dpi, 96);
    limits->ptMinTrackSize.y = MulDiv(140, dpi, 96);
    return 0;
  }
  if (message == WM_CLOSE) {
    if (close_behavior_ == "tray") {
      HideToTray();
      return 0;
    }
    if (close_behavior_ == "minimize") {
      ShowWindow(hwnd, SW_MINIMIZE);
      return 0;
    }
    if (close_behavior_ != "exit" && window_channel_) {
      window_channel_->InvokeMethod("closeRequested", nullptr);
      return 0;
    }
  }
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

void FlutterWindow::SetMiniMode(bool enabled) {
  if (mini_mode_ == enabled) return;
  const HWND window = GetHandle();
  mini_mode_ = enabled;
  if (enabled) {
    GetWindowPlacement(window, &normal_placement_);
    ShowWindow(window, SW_RESTORE);
    RECT bounds;
    GetWindowRect(window, &bounds);
    const auto dpi = GetDpiForWindow(window);
    SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top,
                 MulDiv(560, dpi, 96), MulDiv(156, dpi, 96), SWP_SHOWWINDOW);
  } else {
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPlacement(window, &normal_placement_);
  }
  if (window_channel_) {
    window_channel_->InvokeMethod("miniModeChanged",
        std::make_unique<flutter::EncodableValue>(enabled));
  }
}

void FlutterWindow::HideToTray() {
  // 图标添加失败时不能隐藏窗口，否则用户再也找不回界面。
  if (!tray_ || !tray_->Show()) return;
  ShowWindow(GetHandle(), SW_HIDE);
}

void FlutterWindow::RestoreFromTray() {
  if (tray_) tray_->Hide();
  const HWND window = GetHandle();
  ShowWindow(window, IsZoomed(window) ? SW_SHOW : SW_RESTORE);
  SetForegroundWindow(window);
}
