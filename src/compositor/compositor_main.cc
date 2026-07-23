#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "src/common/logging.h"
#include "src/common/protocol.h"
#include "src/compositor/compositor_configuration.h"
#include "src/compositor/compositor_renderer.h"
#include "src/compositor/layout_server.h"
#include "src/viewer/stream_client.h"

namespace {

constexpr wchar_t kWindowClass[] = L"StreamingBrowserCompositorWindow";
constexpr UINT kRingMessage = WM_APP + 1;
constexpr UINT kFrameMessage = WM_APP + 2;
constexpr UINT kConnectionMessage = WM_APP + 3;
constexpr UINT kLayoutMessage = WM_APP + 4;

struct WindowState {
  HWND window = nullptr;
  streaming::compositor::CompositorConfiguration configuration;
  streaming::compositor::CompositorRenderer renderer;
  std::unique_ptr<streaming::viewer::StreamClient> client;
  std::unique_ptr<streaming::compositor::LayoutServer> layout_server;
  bool connected = false;
  bool renderer_ready = false;
  bool needs_render = true;
  bool fullscreen = false;
  bool server_scaling = false;
  unsigned client_width = 0;
  unsigned client_height = 0;
  std::uint32_t requested_width = 0;
  std::uint32_t requested_height = 0;
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
};

std::wstring Utf8ToWide(std::string_view value);

std::uint16_t CefModifiers() {
  std::uint16_t modifiers = 0;
  if (GetKeyState(VK_SHIFT) < 0) modifiers |= 1U << 1;
  if (GetKeyState(VK_CONTROL) < 0) modifiers |= 1U << 2;
  if (GetKeyState(VK_MENU) < 0) modifiers |= 1U << 3;
  if (GetKeyState(VK_LBUTTON) < 0) modifiers |= 1U << 4;
  if (GetKeyState(VK_MBUTTON) < 0) modifiers |= 1U << 5;
  if (GetKeyState(VK_RBUTTON) < 0) modifiers |= 1U << 6;
  return modifiers;
}

void SendMouse(WindowState* state,
               streaming::protocol::InputKind kind,
               LPARAM lparam,
               int value1 = 0,
               int value2 = 0) {
  if (!state->client || !state->connected) return;
  int x = 0;
  int y = 0;
  if (!state->renderer.WindowToSource(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam),
                                      &x, &y)) {
    return;
  }
  streaming::protocol::InputEvent event;
  event.kind = kind;
  event.modifiers = CefModifiers();
  event.x = x;
  event.y = y;
  event.value1 = value1;
  event.value2 = value2;
  state->client->SendInput(event);
}

void SetFullscreen(WindowState* state, bool fullscreen) {
  if (state->fullscreen == fullscreen) return;
  state->fullscreen = fullscreen;
  if (fullscreen) {
    state->placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(state->window, &state->placement);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(state->window, MONITOR_DEFAULTTONEAREST),
                    &monitor);
    SetWindowLongPtrW(state->window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(state->window, HWND_TOP, monitor.rcMonitor.left,
                 monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_FRAMECHANGED);
  } else {
    SetWindowLongPtrW(state->window, GWL_STYLE,
                      WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetWindowPlacement(state->window, &state->placement);
    SetWindowPos(state->window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  state->needs_render = true;
}

void UpdateWindowTitle(WindowState* state) {
  const std::wstring connection =
      state->connected ? L"overlay connected" : L"waiting for overlay producer";
  const std::wstring scaling =
      state->server_scaling ? L"server scaling" : L"client scaling";
  SetWindowTextW(state->window,
                 (L"Streaming Compositor — " + connection + L" — " + scaling)
                     .c_str());
}

void SendScalingViewport(WindowState* state) {
  if (!state->client || !state->connected) return;
  const std::uint32_t width = state->server_scaling
                                  ? std::max(state->client_width, 320U)
                                  : state->configuration.output_width;
  const std::uint32_t height = state->server_scaling
                                   ? std::max(state->client_height, 240U)
                                   : state->configuration.output_height;
  if (state->requested_width == width && state->requested_height == height) {
    return;
  }
  if (state->client->SendViewportSize(width, height)) {
    state->requested_width = width;
    state->requested_height = height;
    streaming::Log(
        streaming::LogLevel::kInfo,
        std::wstring(L"Compositor switched to ") +
            (state->server_scaling ? L"server" : L"client") +
            L" scaling; requested CEF viewport " + std::to_wstring(width) +
            L"x" + std::to_wstring(height));
  }
}

void SetServerScaling(WindowState* state, bool enabled) {
  if (state->server_scaling == enabled) return;
  state->server_scaling = enabled;
  state->renderer.SetServerScaling(enabled);
  state->requested_width = 0;
  state->requested_height = 0;
  UpdateWindowTitle(state);
  SendScalingViewport(state);
}

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
      state->window = window;
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(state));
      return 0;
    }
    case kRingMessage: {
      std::unique_ptr<streaming::protocol::RingDefinition> definition(
          reinterpret_cast<streaming::protocol::RingDefinition*>(lparam));
      const bool accepted = state->renderer.OpenRing(*definition);
      state->client->AcceptRing(accepted);
      state->needs_render = true;
      if (!accepted) {
        SetWindowTextW(window,
                       L"Streaming Compositor — overlay ring rejected");
      }
      return 0;
    }
    case kFrameMessage: {
      std::unique_ptr<streaming::protocol::FrameMetadata> metadata(
          reinterpret_cast<streaming::protocol::FrameMetadata*>(lparam));
      if (state->renderer.ConsumeFrame(*metadata)) {
        state->client->ReleaseFrame(metadata->slot, metadata->frame_id);
        state->needs_render = true;
      }
      return 0;
    }
    case kConnectionMessage:
      state->connected = wparam != 0;
      if (!state->connected) {
        state->requested_width = 0;
        state->requested_height = 0;
      }
      UpdateWindowTitle(state);
      if (state->connected && state->server_scaling) {
        SendScalingViewport(state);
      }
      return 0;
    case kLayoutMessage: {
      std::unique_ptr<streaming::compositor::LayoutSnapshot> snapshot(
          reinterpret_cast<streaming::compositor::LayoutSnapshot*>(lparam));
      streaming::Log(
        streaming::LogLevel::kInfo,
        L"Compositor applied layout revision " +
          std::to_wstring(snapshot->revision) + L" with " +
          std::to_wstring(snapshot->viewports.size()) + L" viewports");
        if (!snapshot->viewports.empty()) {
        streaming::Log(
          streaming::LogLevel::kInfo,
          L"Compositor layout sources: " +
            Utf8ToWide(snapshot->viewports.front().label) + L" ... " +
            Utf8ToWide(snapshot->viewports.back().label));
        }
      state->renderer.SetLayout(std::move(*snapshot));
      state->needs_render = true;
      return 0;
    }
    case WM_SIZE:
      if (state != nullptr && state->renderer_ready &&
          wparam != SIZE_MINIMIZED) {
        const unsigned width = std::max<unsigned>(LOWORD(lparam), 1U);
        const unsigned height = std::max<unsigned>(HIWORD(lparam), 1U);
        state->client_width = width;
        state->client_height = height;
        state->renderer.Resize(width, height);
        if (state->server_scaling) {
          // The producer keeps its shared ring allocated at the maximum
          // output size, so continuous per-WM_SIZE viewport updates only
          // resize the CEF view; no ring teardown or reconnect happens.
          SendScalingViewport(state);
        }
        state->needs_render = true;
      }
      return 0;
    case WM_TIMER:
      if (state->needs_render && IsWindowVisible(window)) {
        state->needs_render = false;
        state->renderer.Render();
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(window, &paint);
      EndPaint(window, &paint);
      state->renderer.Render();
      return 0;
    }
    case WM_MOUSEMOVE:
      SendMouse(state, streaming::protocol::InputKind::kMouseMove, lparam);
      return 0;
    case WM_LBUTTONDOWN:
      SetFocus(window);
      SendMouse(state, streaming::protocol::InputKind::kMouseDown, lparam, 0, 1);
      return 0;
    case WM_LBUTTONUP:
      SendMouse(state, streaming::protocol::InputKind::kMouseUp, lparam, 0, 1);
      return 0;
    case WM_MOUSEWHEEL: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &point);
      SendMouse(state, streaming::protocol::InputKind::kMouseWheel,
                MAKELPARAM(point.x, point.y), 0,
                GET_WHEEL_DELTA_WPARAM(wparam));
      return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      if (state->client && state->connected) {
        streaming::protocol::InputEvent event;
        event.kind = streaming::protocol::InputKind::kFocus;
        event.value1 = message == WM_SETFOCUS ? 1 : 0;
        state->client->SendInput(event);
      }
      return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
      if (message == WM_KEYDOWN && wparam == VK_F11) {
        SetFullscreen(state, !state->fullscreen);
        return 0;
      }
      if (message == WM_KEYDOWN && wparam == VK_F12) {
        SetServerScaling(state, !state->server_scaling);
        return 0;
      }
      if (state->client && state->connected) {
        streaming::protocol::InputEvent event;
        event.kind = message == WM_KEYDOWN
                         ? streaming::protocol::InputKind::kKeyDown
                         : message == WM_KEYUP
                               ? streaming::protocol::InputKind::kKeyUp
                               : streaming::protocol::InputKind::kCharacter;
        event.modifiers = CefModifiers();
        event.value1 = static_cast<std::int32_t>(wparam);
        event.value2 = static_cast<std::int32_t>(lparam);
        state->client->SendInput(event);
      }
      return 0;
    case WM_DESTROY:
      KillTimer(window, 1);
      KillTimer(window, 2);
      if (state->layout_server) state->layout_server->Stop();
      if (state->client) state->client->Stop();
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), count);
  return result;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPTSTR, int show_command) {
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  std::filesystem::path config_path;
  for (int index = 1; index < argument_count; ++index) {
    const std::wstring_view argument(arguments[index]);
    constexpr std::wstring_view prefix = L"--config=";
    if (argument.starts_with(prefix)) config_path = argument.substr(prefix.size());
  }
  if (arguments != nullptr) LocalFree(arguments);
  if (config_path.empty()) {
    MessageBoxW(nullptr, L"streaming_compositor.exe requires --config=<path>",
                L"Streaming Compositor", MB_OK | MB_ICONERROR);
    return 3;
  }

  auto state = std::make_unique<WindowState>();
  std::string config_error;
  if (!streaming::compositor::LoadCompositorConfigurationYaml(
          config_path, &state->configuration, &config_error)) {
    const std::wstring message = Utf8ToWide(config_error);
    MessageBoxW(nullptr, message.c_str(), L"Streaming Compositor configuration",
                MB_OK | MB_ICONERROR);
    return 3;
  }

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = &WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) return 1;

  HWND window = CreateWindowExW(
      0, kWindowClass, L"Streaming Compositor — waiting for overlay producer",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
      state->configuration.window_width, state->configuration.window_height,
      nullptr, nullptr, instance, state.get());
  if (window == nullptr) return 2;
  if (!state->renderer.Initialize(window, state->configuration.output_width,
                                  state->configuration.output_height)) {
    DestroyWindow(window);
    return 2;
  }
  state->renderer_ready = true;
    RECT client_rect{};
    GetClientRect(window, &client_rect);
    state->client_width =
      static_cast<unsigned>(std::max(client_rect.right - client_rect.left, 1L));
    state->client_height =
      static_cast<unsigned>(std::max(client_rect.bottom - client_rect.top, 1L));
  SetTimer(window, 1, 16, nullptr);

  state->client = std::make_unique<streaming::viewer::StreamClient>(
      [window](streaming::protocol::RingDefinition definition) {
        PostMessageW(window, kRingMessage, 0,
                     reinterpret_cast<LPARAM>(new streaming::protocol::RingDefinition(
                         std::move(definition))));
      },
      [window](streaming::protocol::FrameMetadata metadata) {
        PostMessageW(window, kFrameMessage, 0,
                     reinterpret_cast<LPARAM>(new streaming::protocol::FrameMetadata(
                         std::move(metadata))));
      },
      [](streaming::viewer::NavigationState) {},
      [window](bool connected) {
        PostMessageW(window, kConnectionMessage, connected ? 1 : 0, 0);
      },
      [](bool) {}, [](std::uint32_t) {});
  state->client->Start();

  state->layout_server = std::make_unique<streaming::compositor::LayoutServer>(
      streaming::compositor::LayoutServerOptions{
          state->configuration.bind_address,
          state->configuration.websocket_port,
          state->configuration.websocket_path},
      [window](streaming::compositor::LayoutSnapshot snapshot) {
        PostMessageW(window, kLayoutMessage, 0,
                     reinterpret_cast<LPARAM>(
                         new streaming::compositor::LayoutSnapshot(
                             std::move(snapshot))));
      },
      [](std::wstring message) {
        streaming::Log(streaming::LogLevel::kInfo, std::move(message));
      });
  std::wstring layout_error;
  if (!state->layout_server->Start(&layout_error)) {
    MessageBoxW(window, layout_error.c_str(), L"Layout server",
                MB_OK | MB_ICONERROR);
    DestroyWindow(window);
    return 4;
  }

  ShowWindow(window, state->configuration.maximized ? SW_MAXIMIZE : show_command);
  if (state->configuration.fullscreen) SetFullscreen(state.get(), true);
  UpdateWindow(window);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
