#include "experiments/usb-routing/src/raw_input_capture.h"

#include "experiments/usb-routing/src/emergency_chord.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace streaming::experiments::usb_routing {
namespace {

constexpr wchar_t kWindowClassName[] = L"StreamingBrowserUsbRoutingRawInput";
constexpr UINT kStopMessage = WM_APP + 1;
constexpr std::uint16_t kF8ScanCode = 0x42;

const RAWINPUT* NextRawInputBlock(const RAWINPUT* current) {
  constexpr std::uintptr_t kAlignment = sizeof(std::uint64_t);
  const std::uintptr_t unaligned =
      reinterpret_cast<std::uintptr_t>(current) + current->header.dwSize;
  const std::uintptr_t aligned =
      (unaligned + kAlignment - 1U) & ~(kAlignment - 1U);
  return reinterpret_cast<const RAWINPUT*>(aligned);
}

std::wstring LastErrorMessage(std::wstring_view operation) {
  const DWORD error = GetLastError();
  wchar_t* message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
  std::wstring result(operation);
  result.append(L" failed (");
  result.append(std::to_wstring(error));
  result.append(L")");
  if (length != 0 && message != nullptr) {
    result.append(L": ");
    result.append(message, length);
  }
  if (message != nullptr) {
    LocalFree(message);
  }
  return result;
}

std::wstring RawInputDevicePath(HANDLE device) {
  UINT character_count = 0;
  if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr,
                             &character_count) == static_cast<UINT>(-1) ||
      character_count == 0) {
    return {};
  }
  std::vector<wchar_t> value(static_cast<std::size_t>(character_count) + 1U);
  UINT capacity = static_cast<UINT>(value.size());
  const UINT copied = GetRawInputDeviceInfoW(device, RIDI_DEVICENAME,
                                              value.data(), &capacity);
  if (copied == static_cast<UINT>(-1) || copied == 0) {
    return {};
  }
  return std::wstring(value.data(), copied);
}

}  // namespace

class RawInputCapture::Impl final {
 public:
  struct StartResult {
    bool success = false;
    std::wstring error;
  };

  struct DeviceRecord {
    DeviceIdentity identity;
    DeviceKind kind = DeviceKind::kKeyboard;
    std::uint64_t routed_event_count = 0;
  };

  enum class PendingBinding {
    kKeyboard,
    kMouse,
  };

  bool Start(StatusCallback status,
             EmergencyStopCallback emergency_stop,
             EventCallback event_callback,
             RouteChangedCallback route_changed,
             std::wstring* error) {
    if (thread_.joinable()) {
      if (error != nullptr) {
        *error = L"Raw Input capture is already running";
      }
      return false;
    }
    status_ = std::move(status);
    emergency_stop_ = std::move(emergency_stop);
    event_callback_ = std::move(event_callback);
    route_changed_ = std::move(route_changed);

    std::promise<StartResult> started;
    std::future<StartResult> result = started.get_future();
    thread_ = std::thread(&Impl::ThreadMain, this, std::move(started));
    StartResult start_result = result.get();
    if (!start_result.success) {
      thread_.join();
      if (error != nullptr) {
        *error = std::move(start_result.error);
      }
      return false;
    }
    return true;
  }

  void Stop() {
    if (!thread_.joinable()) {
      return;
    }
    const HWND window = window_.load(std::memory_order_acquire);
    if (window != nullptr) {
      PostMessageW(window, kStopMessage, 0, 0);
    }
    thread_.join();
    window_.store(nullptr, std::memory_order_release);
  }

  std::vector<DeviceSnapshot> Devices() const {
    std::lock_guard lock(mutex_);
    std::vector<DeviceSnapshot> result;
    result.reserve(devices_.size());
    for (const auto& [handle, record] : devices_) {
      static_cast<void>(handle);
      result.push_back({record.identity, record.kind,
                        routed_groups_.contains(record.identity.group_key),
                        record.routed_event_count});
    }
    std::ranges::sort(result, [](const DeviceSnapshot& left,
                                const DeviceSnapshot& right) {
      if (left.kind != right.kind) {
        return left.kind < right.kind;
      }
      if (left.identity.bus != right.identity.bus) {
        return left.identity.bus < right.identity.bus;
      }
      return left.identity.display_name < right.identity.display_name;
    });
    return result;
  }

  void ArmKeyboardBinding() {
    std::lock_guard lock(mutex_);
    pending_binding_ = PendingBinding::kKeyboard;
  }

  void ArmMouseBinding() {
    std::lock_guard lock(mutex_);
    pending_binding_ = PendingBinding::kMouse;
  }

  void CancelBinding() {
    std::lock_guard lock(mutex_);
    pending_binding_.reset();
  }

  void UnrouteAll() {
    bool notify = false;
    {
      std::lock_guard lock(mutex_);
      pending_binding_.reset();
      notify = !routed_groups_.empty();
      routed_groups_.clear();
      emergency_chord_.Clear();
      for (auto& [handle, record] : devices_) {
        static_cast<void>(handle);
        record.routed_event_count = 0;
      }
    }
    if (notify && route_changed_) route_changed_(false);
  }

 private:
  static LRESULT CALLBACK WindowProcedure(HWND window,
                                          UINT message,
                                          WPARAM wparam,
                                          LPARAM lparam) {
    Impl* self = reinterpret_cast<Impl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      self = static_cast<Impl*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
      return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
      case WM_INPUT:
        self->HandleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
        self->DrainRawInputBuffer();
        return DefWindowProcW(window, message, wparam, lparam);
      case WM_INPUT_DEVICE_CHANGE:
        self->HandleDeviceChange(wparam, reinterpret_cast<HANDLE>(lparam));
        return 0;
      case kStopMessage:
        DestroyWindow(window);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
  }

  void ThreadMain(std::promise<StartResult> started) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = &Impl::WindowProcedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      started.set_value({false, LastErrorMessage(L"RegisterClassExW")});
      return;
    }

    const HWND window = CreateWindowExW(
        0, kWindowClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
        instance, this);
    if (window == nullptr) {
      started.set_value({false, LastErrorMessage(L"CreateWindowExW")});
      return;
    }
    window_.store(window, std::memory_order_release);

    RAWINPUTDEVICE registrations[2]{};
    registrations[0].usUsagePage = 0x01;
    registrations[0].usUsage = 0x06;  // Keyboard.
    registrations[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    registrations[0].hwndTarget = window;
    registrations[1].usUsagePage = 0x01;
    registrations[1].usUsage = 0x02;  // Mouse.
    registrations[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    registrations[1].hwndTarget = window;
    if (!RegisterRawInputDevices(registrations, 2,
                                 sizeof(RAWINPUTDEVICE))) {
      const std::wstring failure =
          LastErrorMessage(L"RegisterRawInputDevices");
      DestroyWindow(window);
      started.set_value({false, failure});
      return;
    }

    RefreshDevices();
    started.set_value({true, {}});

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    RAWINPUTDEVICE removals[2]{};
    removals[0].usUsagePage = 0x01;
    removals[0].usUsage = 0x06;
    removals[0].dwFlags = RIDEV_REMOVE;
    removals[1].usUsagePage = 0x01;
    removals[1].usUsage = 0x02;
    removals[1].dwFlags = RIDEV_REMOVE;
    RegisterRawInputDevices(removals, 2, sizeof(RAWINPUTDEVICE));
  }

  void RefreshDevices() {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) ==
        static_cast<UINT>(-1)) {
      EmitStatus(LastErrorMessage(L"GetRawInputDeviceList(size)"));
      return;
    }
    std::vector<RAWINPUTDEVICELIST> list(count);
    if (count != 0 &&
        GetRawInputDeviceList(list.data(), &count,
                              sizeof(RAWINPUTDEVICELIST)) ==
            static_cast<UINT>(-1)) {
      EmitStatus(LastErrorMessage(L"GetRawInputDeviceList(data)"));
      return;
    }

    for (UINT index = 0; index < count; ++index) {
      if (list[index].dwType == RIM_TYPEKEYBOARD ||
          list[index].dwType == RIM_TYPEMOUSE) {
        EnsureDevice(list[index].hDevice, list[index].dwType);
      }
    }
  }

  bool EnsureDevice(HANDLE device, DWORD raw_type) {
    {
      std::lock_guard lock(mutex_);
      if (devices_.contains(device)) {
        return false;
      }
    }

    const std::wstring path = RawInputDevicePath(device);
    if (path.empty()) {
      return false;
    }
    DeviceRecord record;
    record.identity = ResolveDeviceIdentity(path);
    record.kind = raw_type == RIM_TYPEMOUSE ? DeviceKind::kMouse
                                            : DeviceKind::kKeyboard;

    std::lock_guard lock(mutex_);
    return devices_.try_emplace(device, std::move(record)).second;
  }

  void HandleDeviceChange(WPARAM change, HANDLE device) {
    if (change == GIDC_ARRIVAL) {
      RID_DEVICE_INFO info{};
      info.cbSize = sizeof(info);
      UINT size = sizeof(info);
      if (GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &info, &size) !=
              static_cast<UINT>(-1) &&
          (info.dwType == RIM_TYPEKEYBOARD || info.dwType == RIM_TYPEMOUSE)) {
        if (EnsureDevice(device, info.dwType)) {
          EmitStatus(L"Input device arrived; press L to inspect it.");
        }
      }
      return;
    }
    if (change != GIDC_REMOVAL) {
      return;
    }

    std::wstring status;
    bool route_deactivated = false;
    {
      std::lock_guard lock(mutex_);
      const auto found = devices_.find(device);
      if (found == devices_.end()) {
        return;
      }
      const DeviceKey group_key = found->second.identity.group_key;
      const std::wstring name = found->second.identity.display_name;
      devices_.erase(found);
      const bool group_still_present = std::ranges::any_of(
          devices_, [group_key](const auto& entry) {
            return entry.second.identity.group_key == group_key;
          });
      if (!group_still_present) {
        const bool removed = routed_groups_.erase(group_key) != 0;
        emergency_chord_.ForgetDevice(group_key);
        route_deactivated = removed && routed_groups_.empty();
      }
      status = L"Input device removed: " + name;
      if (!group_still_present) {
        status.append(L"; its route and held state were revoked.");
      }
    }
    EmitStatus(std::move(status));
    if (route_deactivated && route_changed_) route_changed_(false);
  }

  void HandleRawInput(HRAWINPUT input_handle) {
    UINT size = 0;
    if (GetRawInputData(input_handle, RID_INPUT, nullptr, &size,
                        sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
        size == 0) {
      return;
    }
    std::vector<std::byte> bytes(size);
    UINT capacity = size;
    if (GetRawInputData(input_handle, RID_INPUT, bytes.data(), &capacity,
                        sizeof(RAWINPUTHEADER)) != size) {
      return;
    }
    ProcessRawInput(*reinterpret_cast<const RAWINPUT*>(bytes.data()));
  }

  void DrainRawInputBuffer() {
    for (;;) {
      UINT size = 0;
      const UINT probe =
          GetRawInputBuffer(nullptr, &size, sizeof(RAWINPUTHEADER));
      if (probe == static_cast<UINT>(-1) || size == 0) {
        return;
      }
      std::vector<std::byte> bytes(size);
      UINT capacity = size;
      const UINT count = GetRawInputBuffer(
          reinterpret_cast<PRAWINPUT>(bytes.data()), &capacity,
          sizeof(RAWINPUTHEADER));
      if (count == static_cast<UINT>(-1) || count == 0) {
        return;
      }
      const RAWINPUT* current =
          reinterpret_cast<const RAWINPUT*>(bytes.data());
      for (UINT index = 0; index < count; ++index) {
        ProcessRawInput(*current);
        current = NextRawInputBlock(current);
      }
    }
  }

  void ProcessRawInput(const RAWINPUT& input) {
    if (input.header.dwType != RIM_TYPEKEYBOARD &&
        input.header.dwType != RIM_TYPEMOUSE) {
      return;
    }
    EnsureDevice(input.header.hDevice, input.header.dwType);

    bool emergency_stop = false;
    std::optional<std::wstring> status;
    std::vector<input_routing::RoutedEvent> routed_events;
    bool route_changed = false;
    {
      std::lock_guard lock(mutex_);
      const auto found = devices_.find(input.header.hDevice);
      if (found == devices_.end()) {
        return;
      }
      DeviceRecord& record = found->second;
      const DeviceKey group_key = record.identity.group_key;

      if (input.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& keyboard = input.data.keyboard;
        const bool key_down = (keyboard.Flags & RI_KEY_BREAK) == 0;
        if (pending_binding_ == PendingBinding::kKeyboard && key_down &&
            keyboard.MakeCode == kF8ScanCode) {
          if (!IsDefaultRoutableBus(record.identity.bus)) {
            status = L"Refused non-USB keyboard: " +
                     record.identity.display_name + L" [" +
                     std::wstring(BusKindName(record.identity.bus)) +
                     L"]. Binding remains armed for a USB keyboard.";
          } else {
            const bool route_was_empty = routed_groups_.empty();
            routed_groups_.insert(group_key);
            pending_binding_.reset();
            route_changed = route_was_empty;
            status = L"Routed keyboard group: " +
                     record.identity.display_name + L" [USB]. Emergency stop "
                     L"is Ctrl+Alt+Shift+F12 on this keyboard.";
          }
        }

        const bool routed = routed_groups_.contains(group_key);
        if (routed) {
          ++record.routed_event_count;
        }
        emergency_stop = emergency_chord_.OnKey(
            group_key, keyboard.MakeCode, keyboard.Flags, key_down, routed);
        if (routed) {
          input_routing::RoutedEvent event;
          event.kind = key_down ? input_routing::EventKind::kKeyDown
                                : input_routing::EventKind::kKeyUp;
          event.device_key = group_key;
          event.timestamp_us = MonotonicMicroseconds();
          event.value1 = static_cast<std::int32_t>(keyboard.VKey);
          event.value2 = 1 |
                         (static_cast<std::int32_t>(keyboard.MakeCode) << 16);
          if ((keyboard.Flags & RI_KEY_E0) != 0)
            event.value2 |= 1 << 24;
          if (!key_down)
            event.value2 |= static_cast<std::int32_t>(0xC0000000U);
          if ((keyboard.Flags & RI_KEY_E0) != 0)
            event.flags |= input_routing::kEventFlagExtended0;
          if ((keyboard.Flags & RI_KEY_E1) != 0)
            event.flags |= input_routing::kEventFlagExtended1;
          event.modifiers = ModifierMask();
          routed_events.push_back(event);
        }
      } else {
        const RAWMOUSE& mouse = input.data.mouse;
        if (pending_binding_ == PendingBinding::kMouse &&
            (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0) {
          if (!IsDefaultRoutableBus(record.identity.bus)) {
            status = L"Refused non-USB mouse: " +
                     record.identity.display_name + L" [" +
                     std::wstring(BusKindName(record.identity.bus)) +
                     L"]. Binding remains armed for a USB mouse.";
          } else {
            const bool route_was_empty = routed_groups_.empty();
            routed_groups_.insert(group_key);
            pending_binding_.reset();
            route_changed = route_was_empty;
            status = L"Routed mouse group: " + record.identity.display_name +
                     L" [USB].";
          }
        }
        if (routed_groups_.contains(group_key)) {
          ++record.routed_event_count;
          AppendMouseEvents(record.identity.group_key, mouse, &routed_events);
        }
      }
    }

    if (status.has_value()) {
      EmitStatus(std::move(*status));
    }
    if (route_changed && route_changed_) route_changed_(true);
    if (event_callback_) {
      for (input_routing::RoutedEvent& event : routed_events)
        event_callback_(std::move(event));
    }
    if (emergency_stop) {
      EmitStatus(L"Emergency stop chord received from a routed keyboard.");
      if (emergency_stop_) {
        emergency_stop_();
      }
    }
  }

  void EmitStatus(std::wstring message) const {
    if (status_) {
      status_(std::move(message));
    }
  }

  static std::uint64_t MonotonicMicroseconds() {
    static const std::uint64_t frequency = [] {
      LARGE_INTEGER value{};
      QueryPerformanceFrequency(&value);
      return static_cast<std::uint64_t>(value.QuadPart);
    }();
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart) * 1'000'000ULL /
           frequency;
  }

  static std::uint32_t ModifierMask() {
    std::uint32_t modifiers = 0;
    if (GetKeyState(VK_SHIFT) < 0) modifiers |= 1U << 1;
    if (GetKeyState(VK_CONTROL) < 0) modifiers |= 1U << 2;
    if (GetKeyState(VK_MENU) < 0) modifiers |= 1U << 3;
    return modifiers;
  }

  static void AppendMouseEvents(
      DeviceKey device_key,
      const RAWMOUSE& mouse,
      std::vector<input_routing::RoutedEvent>* events) {
    const std::uint64_t timestamp = MonotonicMicroseconds();
    if (mouse.lLastX != 0 || mouse.lLastY != 0) {
      input_routing::RoutedEvent event;
      event.kind = input_routing::EventKind::kMouseMove;
      event.device_key = device_key;
      event.timestamp_us = timestamp;
      event.value1 = mouse.lLastX;
      event.value2 = mouse.lLastY;
      event.modifiers = ModifierMask();
      events->push_back(event);
    }
    constexpr std::array pairs{
        std::pair{RI_MOUSE_LEFT_BUTTON_DOWN, 0},
        std::pair{RI_MOUSE_LEFT_BUTTON_UP, 0},
        std::pair{RI_MOUSE_RIGHT_BUTTON_DOWN, 2},
        std::pair{RI_MOUSE_RIGHT_BUTTON_UP, 2},
        std::pair{RI_MOUSE_MIDDLE_BUTTON_DOWN, 1},
        std::pair{RI_MOUSE_MIDDLE_BUTTON_UP, 1},
    };
    for (std::size_t index = 0; index < pairs.size(); ++index) {
      if ((mouse.usButtonFlags & pairs[index].first) == 0) continue;
      input_routing::RoutedEvent event;
      event.kind = (index % 2 == 0)
                       ? input_routing::EventKind::kMouseButtonDown
                       : input_routing::EventKind::kMouseButtonUp;
      event.device_key = device_key;
      event.timestamp_us = timestamp;
      event.value1 = pairs[index].second;
      event.modifiers = ModifierMask();
      events->push_back(event);
    }
    if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
      input_routing::RoutedEvent event;
      event.kind = input_routing::EventKind::kMouseWheel;
      event.device_key = device_key;
      event.timestamp_us = timestamp;
      event.value2 = static_cast<SHORT>(mouse.usButtonData);
      event.modifiers = ModifierMask();
      events->push_back(event);
    }
    if ((mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0) {
      input_routing::RoutedEvent event;
      event.kind = input_routing::EventKind::kMouseWheel;
      event.device_key = device_key;
      event.timestamp_us = timestamp;
      event.value1 = static_cast<SHORT>(mouse.usButtonData);
      event.modifiers = ModifierMask();
      events->push_back(event);
    }
  }

  mutable std::mutex mutex_;
  std::unordered_map<HANDLE, DeviceRecord> devices_;
  std::unordered_set<DeviceKey> routed_groups_;
  std::optional<PendingBinding> pending_binding_;
  EmergencyChord emergency_chord_;
  StatusCallback status_;
  EmergencyStopCallback emergency_stop_;
  EventCallback event_callback_;
  RouteChangedCallback route_changed_;
  std::thread thread_;
  std::atomic<HWND> window_ = nullptr;
};

RawInputCapture::RawInputCapture() : impl_(std::make_unique<Impl>()) {}

RawInputCapture::~RawInputCapture() {
  Stop();
}

bool RawInputCapture::Start(StatusCallback status,
                            EmergencyStopCallback emergency_stop,
                            EventCallback event_callback,
                            RouteChangedCallback route_changed,
                            std::wstring* error) {
  return impl_->Start(std::move(status), std::move(emergency_stop),
                      std::move(event_callback), std::move(route_changed),
                      error);
}

void RawInputCapture::Stop() {
  impl_->Stop();
}

std::vector<DeviceSnapshot> RawInputCapture::Devices() const {
  return impl_->Devices();
}

void RawInputCapture::ArmKeyboardBinding() {
  impl_->ArmKeyboardBinding();
}

void RawInputCapture::ArmMouseBinding() {
  impl_->ArmMouseBinding();
}

void RawInputCapture::CancelBinding() {
  impl_->CancelBinding();
}

void RawInputCapture::UnrouteAll() {
  impl_->UnrouteAll();
}

std::wstring_view DeviceKindName(DeviceKind kind) {
  switch (kind) {
    case DeviceKind::kKeyboard:
      return L"keyboard";
    case DeviceKind::kMouse:
      return L"mouse";
  }
  return L"unknown";
}

}  // namespace streaming::experiments::usb_routing
