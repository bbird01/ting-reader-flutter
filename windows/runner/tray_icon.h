#ifndef RUNNER_TRAY_ICON_H_
#define RUNNER_TRAY_ICON_H_

#include <windows.h>

#include <functional>

constexpr UINT kTrayCallbackMessage = WM_APP + 50;

// 托盘图标仅在窗口隐藏到托盘期间存在，窗口还原后即移除。
class TrayIcon {
 public:
  using Action = std::function<void()>;

  TrayIcon(HWND window, Action on_show, Action on_exit);
  ~TrayIcon();

  bool Show();
  void Hide();
  bool visible() const { return visible_; }
  bool HandleMessage(UINT message, LPARAM lparam);

 private:
  void ShowContextMenu();

  // Win32Window 在 WM_DESTROY 中先清空 window_handle_ 再调用 OnDestroy，
  // 因此这里必须持有 HWND 副本才能在销毁阶段移除图标。
  HWND window_;
  Action on_show_;
  Action on_exit_;
  UINT taskbar_created_;
  bool visible_ = false;
};

#endif  // RUNNER_TRAY_ICON_H_