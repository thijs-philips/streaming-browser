#include <windows.h>

#include <memory>

#include "src/common/logging.h"
#include "src/viewer/d3d_renderer.h"

namespace {

constexpr wchar_t kWindowClass[] = L"StreamingBrowserViewerWindow";

struct WindowState {
  streaming::viewer::D3DRenderer renderer;
};

LRESULT CALLBACK WindowProc(HWND window,
                            UINT message,
                            WPARAM wparam,
                            LPARAM lparam) {
  auto* state = reinterpret_cast<WindowState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_CREATE: {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      state = static_cast<WindowState*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(state));
      if (!state->renderer.Initialize(window)) {
        return -1;
      }
      SetTimer(window, 1, 33, nullptr);
      return 0;
    }
    case WM_SIZE:
      if (state != nullptr && wparam != SIZE_MINIMIZED) {
        state->renderer.Resize(LOWORD(lparam), HIWORD(lparam));
      }
      return 0;
    case WM_TIMER:
      if (state != nullptr) {
        state->renderer.RenderDisconnected();
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_DESTROY:
      KillTimer(window, 1);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPTSTR, int show_command) {
  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = &WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) {
    streaming::LogLastError(streaming::LogLevel::kError,
                            L"RegisterClassExW(viewer)");
    return 1;
  }

  auto state = std::make_unique<WindowState>();
  HWND window = CreateWindowExW(
      0, kWindowClass, L"Streaming Browser Viewer — waiting for producer",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760, nullptr,
      nullptr, instance, state.get());
  if (window == nullptr) {
    streaming::LogLastError(streaming::LogLevel::kError,
                            L"CreateWindowExW(viewer)");
    return 2;
  }

  ShowWindow(window, show_command);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
