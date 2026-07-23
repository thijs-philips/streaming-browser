#include "experiments/usb-routing/src/emergency_chord.h"

#include <windows.h>

namespace streaming::experiments::usb_routing {
namespace {

constexpr std::uint16_t kScanControl = 0x1D;
constexpr std::uint16_t kScanAlt = 0x38;
constexpr std::uint16_t kScanLeftShift = 0x2A;
constexpr std::uint16_t kScanRightShift = 0x36;
constexpr std::uint16_t kScanF12 = 0x58;
constexpr std::uint16_t kScanCodeMask = 0x00FF;
constexpr std::uint16_t kExtended0 = 0x0100;
constexpr std::uint16_t kExtended1 = 0x0200;

}  // namespace

bool EmergencyChord::OnKey(DeviceKey keyboard_group,
                           std::uint16_t make_code,
                           std::uint16_t flags,
                           bool key_down,
                           bool routed) {
  if (!routed) {
    ForgetDevice(keyboard_group);
    return false;
  }

  KeyboardState& state = keyboards_[keyboard_group];
  const std::uint16_t scan_code = NormalizeScanCode(make_code, flags);
  if (key_down) {
    state.held_scan_codes.insert(scan_code);
  } else {
    state.held_scan_codes.erase(scan_code);
  }

  const bool held = IsChordHeld(state);
  if (!held) {
    state.latched = false;
    return false;
  }
  if (state.latched) {
    return false;
  }
  state.latched = true;
  return true;
}

void EmergencyChord::ForgetDevice(DeviceKey keyboard_group) {
  keyboards_.erase(keyboard_group);
}

void EmergencyChord::Clear() {
  keyboards_.clear();
}

std::uint16_t EmergencyChord::NormalizeScanCode(std::uint16_t make_code,
                                                std::uint16_t flags) {
  std::uint16_t normalized = make_code & kScanCodeMask;
  if ((flags & RI_KEY_E0) != 0) {
    normalized |= kExtended0;
  }
  if ((flags & RI_KEY_E1) != 0) {
    normalized |= kExtended1;
  }
  return normalized;
}

bool EmergencyChord::IsChordHeld(const KeyboardState& state) {
  return HasBaseScanCode(state, kScanControl) &&
         HasBaseScanCode(state, kScanAlt) &&
         (HasBaseScanCode(state, kScanLeftShift) ||
          HasBaseScanCode(state, kScanRightShift)) &&
         HasBaseScanCode(state, kScanF12);
}

bool EmergencyChord::HasBaseScanCode(const KeyboardState& state,
                                     std::uint16_t make_code) {
  for (const std::uint16_t held : state.held_scan_codes) {
    if ((held & kScanCodeMask) == make_code) {
      return true;
    }
  }
  return false;
}

}  // namespace streaming::experiments::usb_routing
