#pragma once

#include "experiments/usb-routing/src/device_identity.h"
#include "src/input_routing/routing_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace streaming::experiments::usb_routing {

enum class DeviceKind {
  kKeyboard,
  kMouse,
};

struct DeviceSnapshot {
  DeviceIdentity identity;
  DeviceKind kind = DeviceKind::kKeyboard;
  bool routed = false;
  std::uint64_t routed_event_count = 0;
};

class RawInputCapture final {
 public:
  using StatusCallback = std::function<void(std::wstring)>;
  using EmergencyStopCallback = std::function<void()>;
  using EventCallback =
      std::function<void(input_routing::RoutedEvent)>;
  using RouteChangedCallback = std::function<void(bool)>;

  RawInputCapture();
  ~RawInputCapture();

  RawInputCapture(const RawInputCapture&) = delete;
  RawInputCapture& operator=(const RawInputCapture&) = delete;

  bool Start(StatusCallback status,
             EmergencyStopCallback emergency_stop,
             EventCallback event_callback,
             RouteChangedCallback route_changed,
             std::wstring* error);
  void Stop();

  std::vector<DeviceSnapshot> Devices() const;

  // Binding is deliberately activity based. The next F8 key-down or middle
  // mouse-button down identifies the intended physical device group.
  void ArmKeyboardBinding();
  void ArmMouseBinding();
  void CancelBinding();
  void UnrouteAll();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

std::wstring_view DeviceKindName(DeviceKind kind);

}  // namespace streaming::experiments::usb_routing
