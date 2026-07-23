#pragma once

#include "experiments/usb-routing/src/device_identity.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace streaming::experiments::usb_routing {

// Ctrl+Alt+Shift+F12. Every key must be held on the same routed physical
// keyboard group. The chord latches until at least one member is released.
class EmergencyChord final {
 public:
  bool OnKey(DeviceKey keyboard_group,
             std::uint16_t make_code,
             std::uint16_t flags,
             bool key_down,
             bool routed);

  void ForgetDevice(DeviceKey keyboard_group);
  void Clear();

 private:
  struct KeyboardState {
    std::unordered_set<std::uint16_t> held_scan_codes;
    bool latched = false;
  };

  static std::uint16_t NormalizeScanCode(std::uint16_t make_code,
                                         std::uint16_t flags);
  static bool IsChordHeld(const KeyboardState& state);
  static bool HasBaseScanCode(const KeyboardState& state,
                              std::uint16_t make_code);

  std::unordered_map<DeviceKey, KeyboardState> keyboards_;
};

}  // namespace streaming::experiments::usb_routing
