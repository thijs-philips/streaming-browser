#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace streaming::experiments::usb_routing {

using DeviceKey = std::uint64_t;

enum class BusKind {
  kUnknown,
  kUsb,
  kBluetooth,
  kAcpi,
  kI2c,
};

struct DeviceIdentity {
  DeviceKey device_key = 0;
  DeviceKey group_key = 0;
  std::wstring raw_path;
  std::wstring instance_id;
  std::wstring display_name;
  std::wstring topology;
  std::wstring container_id;
  BusKind bus = BusKind::kUnknown;
};

DeviceKey StableKey(std::wstring_view value);
DeviceIdentity ResolveDeviceIdentity(std::wstring_view raw_path);
std::wstring_view BusKindName(BusKind bus);
bool IsDefaultRoutableBus(BusKind bus);

}  // namespace streaming::experiments::usb_routing
