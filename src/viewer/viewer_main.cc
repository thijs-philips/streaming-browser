#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <imm.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "include/internal/cef_types.h"
#include "src/common/configuration.h"
#include "src/common/logging.h"
#include "src/common/protocol.h"
#include "src/viewer/d3d_renderer.h"
#include "src/viewer/stream_client.h"

namespace {

constexpr wchar_t kWindowClass[] = L"StreamingBrowserViewerWindow";
constexpr wchar_t kRenderSurfaceClass[] =
  L"StreamingBrowserViewerRenderSurface";
constexpr UINT kRingMessage = WM_APP + 1;
constexpr UINT kFrameMessage = WM_APP + 2;
constexpr UINT kNavigationMessage = WM_APP + 3;
constexpr UINT kConnectionMessage = WM_APP + 4;
constexpr UINT kVisibilityMessage = WM_APP + 5;
constexpr UINT kCursorMessage = WM_APP + 6;
constexpr int kUrlBarHeight = 38;
constexpr int kBackButton = 100;
constexpr int kForwardButton = 101;
constexpr int kReloadButton = 102;
constexpr int kGoButton = 103;
constexpr int kUrlEdit = 104;
constexpr int kToggleUrlBar = 200;
constexpr int kTogglePixelPerfect = 201;

struct WindowState {
  HWND window = nullptr;
  HWND render_surface = nullptr;
  HWND url_bar_background = nullptr;
  HWND back = nullptr;
  HWND forward = nullptr;
  HWND reload = nullptr;
  HWND go = nullptr;
  HWND url = nullptr;
  HMENU menu_bar = nullptr;
  streaming::viewer::D3DRenderer renderer;
  std::unique_ptr<streaming::viewer::StreamClient> client;
  bool connected = false;
  bool menu_visible = true;
  bool url_bar_visible = true;
  bool url_bar_overlays_content = true;
  bool updating_url = false;
  bool url_editing = false;
  bool needs_render = true;
  bool initial_pixel_perfect = false;
  bool server_scaling = false;
  bool viewport_dirty = false;
  std::uint32_t last_sent_viewport_width = 0;
  std::uint32_t last_sent_viewport_height = 0;
  std::uint64_t last_title_frame = 0;
  std::string startup_navigation;
  HCURSOR page_cursor = LoadCursor(nullptr, IDC_ARROW);
  bool fullscreen = false;
  WINDOWPLACEMENT window_placement{sizeof(WINDOWPLACEMENT)};
};

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

bool PagePoint(WindowState* state, LPARAM lparam, int* x, int* y) {
  const int window_x = GET_X_LPARAM(lparam);
  const int window_y = GET_Y_LPARAM(lparam);
  if (state->url_bar_visible && window_y < kUrlBarHeight) {
    return false;
  }
  const int render_top =
      state->url_bar_visible && !state->url_bar_overlays_content
          ? kUrlBarHeight
          : 0;
  return state->renderer.WindowToSource(window_x, window_y - render_top,
                                        x, y);
}

void SendMouse(WindowState* state,
               streaming::protocol::InputKind kind,
               LPARAM lparam,
               int value1 = 0,
               int value2 = 0) {
  if (!state->client || !state->connected) return;
  int x = 0;
  int y = 0;
  if (!PagePoint(state, lparam, &x, &y)) return;
  streaming::protocol::InputEvent event;
  event.kind = kind;
  event.modifiers = CefModifiers();
  event.x = x;
  event.y = y;
  event.value1 = value1;
  event.value2 = value2;
  state->client->SendInput(event);
}

void NavigateFromEdit(WindowState* state) {
  const int length = GetWindowTextLengthW(state->url);
  std::wstring wide(static_cast<std::size_t>(length) + 1U, L'\0');
  GetWindowTextW(state->url, wide.data(), length + 1);
  wide.resize(static_cast<std::size_t>(length));
  if (wide.empty()) return;
  const int required = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), length,
                                            nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), length, utf8.data(), required,
                      nullptr, nullptr);
  const bool sent = state->client->SendCommand(
      streaming::protocol::MessageType::kNavigate, std::move(utf8));
  streaming::Log(sent ? streaming::LogLevel::kInfo
                      : streaming::LogLevel::kError,
                 (sent ? L"Viewer sent URL navigation command: "
                       : L"Viewer failed URL navigation command: ") +
                     wide);
  state->url_editing = false;
  SetFocus(state->window);
}

std::u16string ReadImeString(HWND window, DWORD index) {
  HIMC context = ImmGetContext(window);
  if (context == nullptr) return {};
  const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
  std::u16string result;
  if (bytes > 0) {
    result.resize(static_cast<std::size_t>(bytes) / sizeof(char16_t));
    ImmGetCompositionStringW(context, index, result.data(), bytes);
  }
  ImmReleaseContext(window, context);
  return result;
}

std::string WideToUtf8(std::wstring_view value) {
  if (value.empty()) return {};
  const int required = WideCharToMultiByte(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
      nullptr, nullptr);
  std::string result(static_cast<std::size_t>(required), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), required, nullptr, nullptr);
  return result;
}

std::wstring Utf8ToWide(std::string_view value) {
  if (value.empty()) return {};
  const int required = MultiByteToWideChar(
      CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), required);
  return result;
}

bool ArgumentValue(std::wstring_view argument,
                   std::wstring_view name,
                   std::wstring_view* value) {
  if (!argument.starts_with(name) || argument.size() <= name.size() ||
      argument[name.size()] != L'=') {
    return false;
  }
  *value = argument.substr(name.size() + 1);
  return true;
}

bool ParseIntegerArgument(std::wstring_view argument,
                          std::wstring_view name,
                          int minimum,
                          int maximum,
                          int* value,
                          std::string* error) {
  std::wstring_view text;
  if (!ArgumentValue(argument, name, &text)) return true;
  const std::string utf8 = WideToUtf8(text);
  int parsed = 0;
  const auto [end, code] =
      std::from_chars(utf8.data(), utf8.data() + utf8.size(), parsed);
  if (code != std::errc{} || end != utf8.data() + utf8.size() ||
      parsed < minimum || parsed > maximum) {
    *error = WideToUtf8(name) + " must be between " +
             std::to_string(minimum) + " and " + std::to_string(maximum);
    return false;
  }
  *value = parsed;
  return true;
}

bool LoadViewerConfiguration(int argument_count,
                             LPWSTR* arguments,
                             streaming::ViewerConfiguration* configuration,
                             std::string* error) {
  for (int i = 1; i < argument_count; ++i) {
    if (std::wstring_view(arguments[i]) == L"--config") {
      *error = "--config requires a YAML file path";
      return false;
    }
    std::wstring_view path;
    if (ArgumentValue(arguments[i], L"--config", &path)) {
      if (!streaming::LoadViewerConfigurationYaml(
              std::filesystem::path(path), configuration, error)) {
        return false;
      }
      streaming::Log(streaming::LogLevel::kInfo,
                     L"Loaded viewer YAML configuration from " +
                         std::wstring(path));
      break;
    }
  }

  for (int i = 1; i < argument_count; ++i) {
    const std::wstring_view argument(arguments[i]);
    std::wstring_view value;
    if (ArgumentValue(argument, L"--navigate", &value)) {
      configuration->navigate = WideToUtf8(value);
    } else if (!ParseIntegerArgument(argument, L"--window-width", 320, 16384,
                                     &configuration->window_width, error) ||
               !ParseIntegerArgument(argument, L"--window-height", 240, 16384,
                                     &configuration->window_height, error)) {
      return false;
    } else if (argument == L"--toolbar") {
      configuration->show_toolbar = true;
    } else if (argument == L"--no-toolbar") {
      configuration->show_toolbar = false;
    } else if (argument == L"--url-bar") {
      configuration->show_url_bar = true;
    } else if (argument == L"--no-url-bar") {
      configuration->show_url_bar = false;
    } else if (argument == L"--url-bar-overlays-content") {
      configuration->url_bar_overlays_content = true;
    } else if (argument == L"--content-below-url-bar") {
      configuration->url_bar_overlays_content = false;
    } else if (argument == L"--pixel-perfect") {
      configuration->pixel_perfect = true;
    } else if (argument == L"--fit") {
      configuration->pixel_perfect = false;
    } else if (argument == L"--server-scaling") {
      configuration->scaling = "server";
    } else if (argument == L"--client-scaling") {
      configuration->scaling = "client";
    } else if (argument == L"--fullscreen") {
      configuration->fullscreen = true;
      configuration->maximized = false;
    } else if (argument == L"--windowed") {
      configuration->fullscreen = false;
    } else if (argument == L"--maximized") {
      configuration->maximized = true;
      configuration->fullscreen = false;
    } else if (argument == L"--no-maximized") {
      configuration->maximized = false;
    }
  }
  return true;
}

void ReportViewportIfChanged(WindowState* state) {
  if (!state->server_scaling || !state->client || !state->connected) {
    return;
  }
  RECT surface{};
  if (!GetClientRect(state->render_surface, &surface)) {
    return;
  }
  const auto width = static_cast<std::uint32_t>(
      std::clamp<LONG>(surface.right - surface.left, 320, 16384));
  const auto height = static_cast<std::uint32_t>(
      std::clamp<LONG>(surface.bottom - surface.top, 240, 16384));
  if (width == state->last_sent_viewport_width &&
      height == state->last_sent_viewport_height) {
    return;
  }
  if (state->client->SendViewportSize(width, height)) {
    state->last_sent_viewport_width = width;
    state->last_sent_viewport_height = height;
    streaming::Log(streaming::LogLevel::kInfo,
                   L"Viewer requested server-side viewport " +
                       std::to_wstring(width) + L"x" +
                       std::to_wstring(height));
  }
}

void SetUrlBarVisible(WindowState* state, bool visible) {
  state->url_bar_visible = visible;
  const int show = visible ? SW_SHOW : SW_HIDE;
  ShowWindow(state->url_bar_background, show);
  ShowWindow(state->back, show);
  ShowWindow(state->forward, show);
  ShowWindow(state->reload, show);
  ShowWindow(state->go, show);
  ShowWindow(state->url, show);
  if (state->menu_bar != nullptr) {
    CheckMenuItem(state->menu_bar, kToggleUrlBar,
                  MF_BYCOMMAND | (visible ? MF_CHECKED : MF_UNCHECKED));
  }
  RECT client{};
  GetClientRect(state->window, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  const int render_top =
      visible && !state->url_bar_overlays_content ? kUrlBarHeight : 0;
  const int render_height = std::max(height - render_top, 1);
  MoveWindow(state->render_surface, 0, render_top, width, render_height, TRUE);
  state->renderer.Resize(static_cast<unsigned>(std::max(width, 1)),
                         static_cast<unsigned>(render_height));
  state->needs_render = true;
}

void SetFullscreen(WindowState* state, bool fullscreen) {
  if (state->fullscreen == fullscreen) return;
  state->fullscreen = fullscreen;
  if (fullscreen) {
    state->window_placement.length = sizeof(WINDOWPLACEMENT);
    GetWindowPlacement(state->window, &state->window_placement);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(state->window, MONITOR_DEFAULTTONEAREST),
                    &monitor);
    SetMenu(state->window, nullptr);
    SetWindowLongPtrW(state->window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(state->window, HWND_TOP, monitor.rcMonitor.left,
                 monitor.rcMonitor.top,
                 monitor.rcMonitor.right - monitor.rcMonitor.left,
                 monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                 SWP_FRAMECHANGED);
  } else {
    SetWindowLongPtrW(state->window, GWL_STYLE,
                      WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    if (state->menu_visible && state->menu_bar != nullptr) {
      SetMenu(state->window, state->menu_bar);
    }
    SetWindowPlacement(state->window, &state->window_placement);
    SetWindowPos(state->window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  state->needs_render = true;
}

void CreateViewerChrome(WindowState* state, HINSTANCE instance) {
  HMENU menu = CreateMenu();
  HMENU view_menu = CreatePopupMenu();
  AppendMenuW(view_menu,
              MF_STRING | (state->url_bar_visible ? MF_CHECKED : 0),
              kToggleUrlBar,
              L"Show URL bar");
  AppendMenuW(view_menu,
              MF_STRING | (state->initial_pixel_perfect ? MF_CHECKED : 0),
              kTogglePixelPerfect,
              L"1:1 pixel mode (Ctrl+Arrows to pan)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"View");
  state->menu_bar = menu;
  if (state->menu_visible) {
    SetMenu(state->window, menu);
  }
  const DWORD control_style =
      WS_CHILD | (state->url_bar_visible ? WS_VISIBLE : 0);
    state->url_bar_background = CreateWindowW(
      L"STATIC", L"", control_style | SS_GRAYRECT,
      0, 0, 1, kUrlBarHeight, state->window, nullptr, instance, nullptr);
  state->back = CreateWindowW(L"BUTTON", L"←", control_style,
                              6, 6, 32, 26, state->window,
                                reinterpret_cast<HMENU>(
                                  static_cast<INT_PTR>(kBackButton)), instance,
                              nullptr);
  state->forward = CreateWindowW(L"BUTTON", L"→", control_style,
                                 42, 6, 32, 26, state->window,
                                 reinterpret_cast<HMENU>(
                                   static_cast<INT_PTR>(kForwardButton)),
                                 instance, nullptr);
  state->reload = CreateWindowW(L"BUTTON", L"Reload", control_style,
                                78, 6, 62, 26, state->window,
                                reinterpret_cast<HMENU>(
                                  static_cast<INT_PTR>(kReloadButton)), instance,
                                nullptr);
  state->go = CreateWindowW(L"BUTTON", L"Go", control_style,
                            510, 6, 42, 26, state->window,
                            reinterpret_cast<HMENU>(
                              static_cast<INT_PTR>(kGoButton)), instance,
                            nullptr);
  state->url = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", control_style | ES_AUTOHSCROLL,
      146, 6, 358, 26, state->window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUrlEdit)),
      instance, nullptr);
  EnableWindow(state->back, FALSE);
  EnableWindow(state->forward, FALSE);
}

LRESULT CALLBACK RenderSurfaceProc(HWND window,
                                   UINT message,
                                   WPARAM wparam,
                                   LPARAM lparam) {
  HWND parent = GetParent(window);
  switch (message) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      MapWindowPoints(window, parent, &point, 1);
      return SendMessageW(parent, message, wparam,
                          MAKELPARAM(point.x, point.y));
    }
    case WM_MOUSEWHEEL:
    case WM_SETCURSOR:
      return SendMessageW(parent, message, wparam, lparam);
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
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
      state->render_surface = CreateWindowExW(
          0, kRenderSurfaceClass, L"",
          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
          0, 0, 1, 1, window, nullptr, create->hInstance, nullptr);
      if (state->render_surface == nullptr) return -1;
      CreateViewerChrome(state, create->hInstance);
      if (!state->renderer.Initialize(state->render_surface)) return -1;
      state->renderer.SetPixelPerfect(state->initial_pixel_perfect);
      SetTimer(window, 1, 16, nullptr);
      return 0;
    }
    case kRingMessage: {
      std::unique_ptr<streaming::protocol::RingDefinition> definition(
          reinterpret_cast<streaming::protocol::RingDefinition*>(lparam));
      const bool opened = state->renderer.OpenRing(*definition);
      state->client->AcceptRing(opened);
      state->needs_render = true;
      state->last_title_frame = 0;
      SetWindowTextW(window, opened ? L"Streaming Browser Viewer — connected"
                                    : L"Streaming Browser Viewer — ring failed");
      return 0;
    }
    case kFrameMessage: {
      std::unique_ptr<streaming::protocol::FrameMetadata> metadata(
          reinterpret_cast<streaming::protocol::FrameMetadata*>(lparam));
      if (state->renderer.ConsumeFrame(*metadata)) {
        state->client->ReleaseFrame(metadata->slot, metadata->frame_id);
        state->needs_render = true;
        // SetWindowText forces a non-client repaint; throttle to ~1 Hz.
        if (metadata->frame_id == 1 ||
            metadata->frame_id >= state->last_title_frame + 30) {
          state->last_title_frame = metadata->frame_id;
          const std::wstring title = L"Streaming Browser Viewer — frame " +
                                     std::to_wstring(metadata->frame_id);
          SetWindowTextW(window, title.c_str());
        }
      } else {
        const std::wstring title =
            L"Streaming Browser Viewer — frame consume failed " +
            std::to_wstring(metadata->frame_id);
        SetWindowTextW(window, title.c_str());
      }
      return 0;
    }
    case kNavigationMessage: {
      std::unique_ptr<streaming::viewer::NavigationState> navigation(
          reinterpret_cast<streaming::viewer::NavigationState*>(lparam));
      const int required = MultiByteToWideChar(
          CP_UTF8, 0, navigation->url.data(),
          static_cast<int>(navigation->url.size()), nullptr, 0);
      std::wstring url(static_cast<std::size_t>(required), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, navigation->url.data(),
                          static_cast<int>(navigation->url.size()), url.data(),
                          required);
      if (!state->url_editing) {
        state->updating_url = true;
        SetWindowTextW(state->url, url.c_str());
        state->updating_url = false;
      }
      EnableWindow(state->back, navigation->can_go_back);
      EnableWindow(state->forward, navigation->can_go_forward);
      SetWindowTextW(state->reload, navigation->loading ? L"Stop" : L"Reload");
      return 0;
    }
    case kConnectionMessage:
      state->connected = wparam != 0;
      if (state->connected && !state->startup_navigation.empty()) {
        state->client->SendCommand(
            streaming::protocol::MessageType::kNavigate,
            std::exchange(state->startup_navigation, {}));
      }
      if (state->connected) {
        // A new session starts from the producer's own size; resend ours.
        state->last_sent_viewport_width = 0;
        state->last_sent_viewport_height = 0;
        state->viewport_dirty = true;
      }
      if (!state->connected) {
        SetWindowTextW(window,
                       L"Streaming Browser Viewer — waiting for producer");
      }
      return 0;
    case kVisibilityMessage:
      ShowWindow(window, wparam != 0 ? SW_SHOW : SW_HIDE);
      if (wparam != 0) {
        state->needs_render = true;
      }
      return 0;
    case kCursorMessage: {
      LPCWSTR cursor_id = IDC_ARROW;
      switch (static_cast<cef_cursor_type_t>(wparam)) {
        case CT_IBEAM: cursor_id = IDC_IBEAM; break;
        case CT_HAND: cursor_id = IDC_HAND; break;
        case CT_CROSS: cursor_id = IDC_CROSS; break;
        case CT_WAIT: cursor_id = IDC_WAIT; break;
        case CT_NORTHSOUTHRESIZE: cursor_id = IDC_SIZENS; break;
        case CT_EASTWESTRESIZE: cursor_id = IDC_SIZEWE; break;
        case CT_NORTHWESTSOUTHEASTRESIZE: cursor_id = IDC_SIZENWSE; break;
        case CT_NORTHEASTSOUTHWESTRESIZE: cursor_id = IDC_SIZENESW; break;
        case CT_MOVE: cursor_id = IDC_SIZEALL; break;
        case CT_NOTALLOWED: cursor_id = IDC_NO; break;
        default: break;
      }
      state->page_cursor = LoadCursor(nullptr, cursor_id);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        SetCursor(state->page_cursor);
        return TRUE;
      }
      break;
    case WM_COMMAND:
      if (LOWORD(wparam) == kToggleUrlBar) {
        SetUrlBarVisible(state, !state->url_bar_visible);
        return 0;
      }
      if (LOWORD(wparam) == kTogglePixelPerfect) {
        state->renderer.SetPixelPerfect(!state->renderer.pixel_perfect());
        state->needs_render = true;
        if (state->menu_bar != nullptr) {
          CheckMenuItem(
              state->menu_bar, kTogglePixelPerfect,
              MF_BYCOMMAND |
                  (state->renderer.pixel_perfect() ? MF_CHECKED
                                                   : MF_UNCHECKED));
        }
        return 0;
      }
      if (LOWORD(wparam) == kUrlEdit && HIWORD(wparam) == EN_CHANGE &&
          !state->updating_url) {
        state->url_editing = true;
        return 0;
      }
      if (HIWORD(wparam) == BN_CLICKED && state->client) {
        switch (LOWORD(wparam)) {
          case kBackButton:
            state->client->SendCommand(streaming::protocol::MessageType::kBack);
            break;
          case kForwardButton:
            state->client->SendCommand(
                streaming::protocol::MessageType::kForward);
            break;
          case kReloadButton: {
            wchar_t text[16]{};
            GetWindowTextW(state->reload, text, 16);
            state->client->SendCommand(
                std::wstring_view(text) == L"Stop"
                    ? streaming::protocol::MessageType::kStopLoad
                    : streaming::protocol::MessageType::kReload);
            break;
          }
          case kGoButton:
            NavigateFromEdit(state);
            break;
        }
      }
      return 0;
    case WM_SIZE:
      if (state != nullptr && wparam != SIZE_MINIMIZED) {
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        const int render_top =
            state->url_bar_visible && !state->url_bar_overlays_content
                ? kUrlBarHeight
                : 0;
        const int render_height = std::max(height - render_top, 1);
        MoveWindow(state->render_surface, 0, render_top, width,
                   render_height, TRUE);
        state->renderer.Resize(static_cast<unsigned>(std::max(width, 1)),
                               static_cast<unsigned>(render_height));
        MoveWindow(state->url_bar_background, 0, 0, width,
                   kUrlBarHeight, TRUE);
        MoveWindow(state->url, 146, 6, std::max(width - 200, 100), 26, TRUE);
        MoveWindow(state->go, std::max(width - 48, 146), 6, 42, 26, TRUE);
        state->needs_render = true;
        state->viewport_dirty = true;
      }
      return 0;
    case WM_TIMER:
      // Debounce server-side viewport reports to the timer tick so drag
      // resizing does not flood the producer with browser resizes.
      if (state->viewport_dirty) {
        state->viewport_dirty = false;
        ReportViewportIfChanged(state);
      }
      // Only present when content actually changed; redundant 4K presents
      // cost measurable GPU and DWM composition time. A hidden window keeps
      // its dirty flag so the first visible frame is current.
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
      SetCapture(window);
      SendMouse(state, streaming::protocol::InputKind::kMouseDown, lparam, 0, 1);
      return 0;
    case WM_LBUTTONUP:
      SendMouse(state, streaming::protocol::InputKind::kMouseUp, lparam, 0, 1);
      ReleaseCapture();
      return 0;
    case WM_MBUTTONDOWN:
      SetCapture(window);
      SendMouse(state, streaming::protocol::InputKind::kMouseDown, lparam, 1, 1);
      return 0;
    case WM_MBUTTONUP:
      SendMouse(state, streaming::protocol::InputKind::kMouseUp, lparam, 1, 1);
      ReleaseCapture();
      return 0;
    case WM_RBUTTONDOWN:
      SetCapture(window);
      SendMouse(state, streaming::protocol::InputKind::kMouseDown, lparam, 2, 1);
      return 0;
    case WM_RBUTTONUP:
      SendMouse(state, streaming::protocol::InputKind::kMouseUp, lparam, 2, 1);
      ReleaseCapture();
      return 0;
    case WM_MOUSEWHEEL: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window, &point);
      const LPARAM client_point = MAKELPARAM(point.x, point.y);
      SendMouse(state, streaming::protocol::InputKind::kMouseWheel, client_point,
                0, GET_WHEEL_DELTA_WPARAM(wparam));
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
      if (message == WM_KEYDOWN && state->renderer.pixel_perfect() &&
          GetKeyState(VK_CONTROL) < 0) {
        switch (wparam) {
          case VK_LEFT:
            state->renderer.Pan(64.0F, 0.0F);
            state->needs_render = true;
            return 0;
          case VK_RIGHT:
            state->renderer.Pan(-64.0F, 0.0F);
            state->needs_render = true;
            return 0;
          case VK_UP:
            state->renderer.Pan(0.0F, 64.0F);
            state->needs_render = true;
            return 0;
          case VK_DOWN:
            state->renderer.Pan(0.0F, -64.0F);
            state->needs_render = true;
            return 0;
        }
      }
      if (state->client && state->connected && GetFocus() == window) {
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
      break;
    case WM_IME_STARTCOMPOSITION:
      return 0;
    case WM_IME_COMPOSITION:
      if (state->client && state->connected) {
        if ((lparam & GCS_RESULTSTR) != 0) {
          state->client->SendIme(
              {streaming::protocol::ImeKind::kCommit,
               ReadImeString(window, GCS_RESULTSTR)});
        }
        if ((lparam & GCS_COMPSTR) != 0) {
          state->client->SendIme(
              {streaming::protocol::ImeKind::kComposition,
               ReadImeString(window, GCS_COMPSTR)});
        }
      }
      return 0;
    case WM_IME_ENDCOMPOSITION:
      if (state->client && state->connected) {
        state->client->SendIme(
            {streaming::protocol::ImeKind::kFinish, {}});
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_DESTROY:
      KillTimer(window, 1);
      if (state->menu_bar != nullptr && GetMenu(window) == nullptr) {
        DestroyMenu(state->menu_bar);
        state->menu_bar = nullptr;
      }
      if (state->client) state->client->Stop();
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPTSTR, int show_command) {
  streaming::ViewerConfiguration configuration;
  int argument_count = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
  std::string configuration_error;
  if (arguments == nullptr ||
      !LoadViewerConfiguration(argument_count, arguments, &configuration,
                               &configuration_error)) {
    if (arguments != nullptr) LocalFree(arguments);
    const std::wstring message =
        L"Could not load viewer configuration: " +
        Utf8ToWide(configuration_error);
    streaming::Log(streaming::LogLevel::kError, message);
    MessageBoxW(nullptr, message.c_str(), L"Streaming Viewer configuration",
                MB_OK | MB_ICONERROR);
    return 3;
  }
  LocalFree(arguments);

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = &WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0) return 1;

  WNDCLASSEXW render_surface_class{sizeof(render_surface_class)};
  render_surface_class.lpfnWndProc = &RenderSurfaceProc;
  render_surface_class.hInstance = instance;
  render_surface_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  render_surface_class.lpszClassName = kRenderSurfaceClass;
  if (RegisterClassExW(&render_surface_class) == 0) return 1;

  auto state = std::make_unique<WindowState>();
  state->startup_navigation = configuration.navigate;
  state->server_scaling = configuration.scaling == "server";
  state->menu_visible = configuration.show_toolbar;
  state->url_bar_visible = configuration.show_url_bar;
  state->url_bar_overlays_content = configuration.url_bar_overlays_content;
  state->initial_pixel_perfect = configuration.pixel_perfect;
  HWND window = CreateWindowExW(
      0, kWindowClass, L"Streaming Browser Viewer — waiting for producer",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
      configuration.window_width, configuration.window_height, nullptr,
      nullptr, instance, state.get());
  if (window == nullptr) return 2;

  state->client = std::make_unique<streaming::viewer::StreamClient>(
      [window](streaming::protocol::RingDefinition definition) {
        PostMessageW(window, kRingMessage, 0,
                     reinterpret_cast<LPARAM>(
                         new streaming::protocol::RingDefinition(
                             std::move(definition))));
      },
      [window](streaming::protocol::FrameMetadata metadata) {
        PostMessageW(window, kFrameMessage, 0,
                     reinterpret_cast<LPARAM>(
                         new streaming::protocol::FrameMetadata(
                             std::move(metadata))));
      },
      [window](streaming::viewer::NavigationState navigation) {
        PostMessageW(window, kNavigationMessage, 0,
                     reinterpret_cast<LPARAM>(
                         new streaming::viewer::NavigationState(
                             std::move(navigation))));
      },
      [window](bool connected) {
        PostMessageW(window, kConnectionMessage, connected ? 1 : 0, 0);
      },
      [window](bool visible) {
        PostMessageW(window, kVisibilityMessage, visible ? 1 : 0, 0);
      },
      [window](std::uint32_t cursor_type) {
        PostMessageW(window, kCursorMessage, cursor_type, 0);
      });
  state->client->Start();

  ShowWindow(window, configuration.maximized ? SW_MAXIMIZE : show_command);
  if (configuration.fullscreen) {
    SetFullscreen(state.get(), true);
  }
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
