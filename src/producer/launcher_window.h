#pragma once

#include <windows.h>

namespace streaming::producer {

inline constexpr UINT kShowLauncherMessage = WM_APP + 1;
inline constexpr UINT kCaptureFrameMessage = WM_APP + 2;
inline constexpr UINT kCaptureFailureMessage = WM_APP + 3;
inline constexpr UINT kStreamFrameMessage = WM_APP + 4;

class LauncherWindow final {
 public:
  using CloseCallback = void (*)(void* context);
  using VisibilityCallback = void (*)(void* context, bool visible);

  LauncherWindow() = default;
  ~LauncherWindow();

  bool Create(HINSTANCE instance,
              CloseCallback close_callback,
              VisibilityCallback visibility_callback,
              void* callback_context,
              bool initially_visible);
  int RunMessageLoop();
  void SetStatus(const wchar_t* status);
  [[nodiscard]] HWND window() const { return window_; }

 private:
  static LRESULT CALLBACK WindowProc(HWND window,
                                     UINT message,
                                     WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);

  HWND window_ = nullptr;
  HWND status_ = nullptr;
  HWND visible_checkbox_ = nullptr;
  CloseCallback close_callback_ = nullptr;
  VisibilityCallback visibility_callback_ = nullptr;
  void* callback_context_ = nullptr;
  bool close_requested_ = false;
};

}  // namespace streaming::producer
