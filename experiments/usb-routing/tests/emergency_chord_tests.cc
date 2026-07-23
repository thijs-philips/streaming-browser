#include "experiments/usb-routing/src/emergency_chord.h"

#include <windows.h>

#include <iostream>
#include <string_view>

namespace usb = streaming::experiments::usb_routing;
namespace {

constexpr usb::DeviceKey kKeyboardA = 100;
constexpr usb::DeviceKey kKeyboardB = 200;

int failures = 0;

void Check(bool condition, std::string_view name) {
  if (!condition) {
    std::cerr << "FAILED: " << name << '\n';
    ++failures;
  }
}

bool Down(usb::EmergencyChord* chord,
          usb::DeviceKey device,
          std::uint16_t scan,
          std::uint16_t flags = 0,
          bool routed = true) {
  return chord->OnKey(device, scan, flags, true, routed);
}

bool Up(usb::EmergencyChord* chord,
        usb::DeviceKey device,
        std::uint16_t scan,
        std::uint16_t flags = 0,
        bool routed = true) {
  return chord->OnKey(device, scan, flags, false, routed);
}

void PressPrefix(usb::EmergencyChord* chord, usb::DeviceKey device) {
  Check(!Down(chord, device, 0x1D), "Control alone does not trigger");
  Check(!Down(chord, device, 0x38), "Control+Alt does not trigger");
  Check(!Down(chord, device, 0x2A), "Control+Alt+Shift does not trigger");
}

void TestSameDeviceTriggersAndLatches() {
  usb::EmergencyChord chord;
  PressPrefix(&chord, kKeyboardA);
  Check(Down(&chord, kKeyboardA, 0x58), "same-device chord triggers");
  Check(!Down(&chord, kKeyboardA, 0x58), "repeat is latched");
  Check(!Up(&chord, kKeyboardA, 0x58), "release does not trigger");
  Check(Down(&chord, kKeyboardA, 0x58), "chord rearms after release");
}

void TestSplitDevicesNeverTrigger() {
  usb::EmergencyChord chord;
  Check(!Down(&chord, kKeyboardA, 0x1D), "split Control");
  Check(!Down(&chord, kKeyboardA, 0x38), "split Alt");
  Check(!Down(&chord, kKeyboardA, 0x2A), "split Shift");
  Check(!Down(&chord, kKeyboardB, 0x58), "F12 on other device ignored");
  Check(!Down(&chord, kKeyboardB, 0x1D), "partial second chord");
  Check(!Down(&chord, kKeyboardB, 0x38), "partial second chord Alt");
}

void TestUnroutedDeviceNeverTriggers() {
  usb::EmergencyChord chord;
  Check(!Down(&chord, kKeyboardA, 0x1D, 0, false), "unrouted Control");
  Check(!Down(&chord, kKeyboardA, 0x38, 0, false), "unrouted Alt");
  Check(!Down(&chord, kKeyboardA, 0x2A, 0, false), "unrouted Shift");
  Check(!Down(&chord, kKeyboardA, 0x58, 0, false), "unrouted F12");
}

void TestExtendedModifiersAndRightShift() {
  usb::EmergencyChord chord;
  Check(!Down(&chord, kKeyboardA, 0x1D, RI_KEY_E0), "right Control");
  Check(!Down(&chord, kKeyboardA, 0x38, RI_KEY_E0), "right Alt");
  Check(!Down(&chord, kKeyboardA, 0x36), "right Shift");
  Check(Down(&chord, kKeyboardA, 0x58),
        "extended modifiers and right Shift trigger");
}

void TestForgetClearsHeldState() {
  usb::EmergencyChord chord;
  PressPrefix(&chord, kKeyboardA);
  chord.ForgetDevice(kKeyboardA);
  Check(!Down(&chord, kKeyboardA, 0x58), "forgotten modifiers do not trigger");
}

void TestDefaultBusPolicyIsUsbOnly() {
  Check(usb::IsDefaultRoutableBus(usb::BusKind::kUsb),
    "USB is routable by default");
  Check(!usb::IsDefaultRoutableBus(usb::BusKind::kBluetooth),
    "Bluetooth is refused by default");
  Check(!usb::IsDefaultRoutableBus(usb::BusKind::kAcpi),
    "ACPI devices are refused by default");
  Check(!usb::IsDefaultRoutableBus(usb::BusKind::kI2c),
    "I2C devices are refused by default");
  Check(!usb::IsDefaultRoutableBus(usb::BusKind::kUnknown),
    "unknown buses are refused by default");
}

}  // namespace

int main() {
  TestSameDeviceTriggersAndLatches();
  TestSplitDevicesNeverTrigger();
  TestUnroutedDeviceNeverTriggers();
  TestExtendedModifiersAndRightShift();
  TestForgetClearsHeldState();
  TestDefaultBusPolicyIsUsbOnly();
  if (failures == 0) {
    std::cout << "Emergency chord tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
