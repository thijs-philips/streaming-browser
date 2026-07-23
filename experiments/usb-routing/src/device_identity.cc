#include "experiments/usb-routing/src/device_identity.h"

#include <windows.h>

#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <objbase.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace streaming::experiments::usb_routing {
namespace {

bool EqualsInsensitive(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                              right.data(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}

bool ContainsInsensitive(std::wstring_view value, std::wstring_view token) {
  if (token.empty() || token.size() > value.size()) {
    return false;
  }
  for (std::size_t offset = 0; offset + token.size() <= value.size(); ++offset) {
    if (EqualsInsensitive(value.substr(offset, token.size()), token)) {
      return true;
    }
  }
  return false;
}

std::wstring GetRegistryString(HDEVINFO devices,
                               SP_DEVINFO_DATA* device,
                               DWORD property) {
  DWORD type = 0;
  DWORD required_size = 0;
  SetupDiGetDeviceRegistryPropertyW(devices, device, property, &type, nullptr,
                                    0, &required_size);
  if (required_size == 0) {
    return {};
  }
  std::vector<BYTE> buffer(required_size + sizeof(wchar_t), 0);
  if (!SetupDiGetDeviceRegistryPropertyW(
          devices, device, property, &type, buffer.data(), required_size,
          nullptr)) {
    return {};
  }
  return reinterpret_cast<const wchar_t*>(buffer.data());
}

std::wstring GetDevicePropertyString(HDEVINFO devices,
                                     SP_DEVINFO_DATA* device,
                                     const DEVPROPKEY& property) {
  DEVPROPTYPE type = 0;
  DWORD required_size = 0;
  SetupDiGetDevicePropertyW(devices, device, &property, &type, nullptr, 0,
                            &required_size, 0);
  if (required_size == 0 || type != DEVPROP_TYPE_STRING) {
    return {};
  }
  std::vector<BYTE> buffer(required_size + sizeof(wchar_t), 0);
  if (!SetupDiGetDevicePropertyW(devices, device, &property, &type,
                                 buffer.data(), required_size, nullptr, 0)) {
    return {};
  }
  return reinterpret_cast<const wchar_t*>(buffer.data());
}

std::wstring GuidString(const GUID& value) {
  std::array<wchar_t, 40> text{};
  const int length = StringFromGUID2(value, text.data(),
                                     static_cast<int>(text.size()));
  if (length <= 1) {
    return {};
  }
  return std::wstring(text.data(), static_cast<std::size_t>(length - 1));
}

std::wstring GetContainerId(HDEVINFO devices, SP_DEVINFO_DATA* device) {
  GUID container{};
  DEVPROPTYPE type = 0;
  DWORD required_size = 0;
  if (!SetupDiGetDevicePropertyW(
          devices, device, &DEVPKEY_Device_ContainerId, &type,
          reinterpret_cast<PBYTE>(&container), sizeof(container),
          &required_size, 0) ||
      type != DEVPROP_TYPE_GUID || required_size != sizeof(container)) {
    return {};
  }
  const bool null_guid =
      container.Data1 == 0 && container.Data2 == 0 && container.Data3 == 0 &&
      std::ranges::all_of(container.Data4,
                          [](const std::uint8_t value) { return value == 0; });
  const bool windows_placeholder =
      container.Data1 == 0 && container.Data2 == 0 && container.Data3 == 0 &&
      std::ranges::all_of(container.Data4,
                          [](const std::uint8_t value) { return value == 0xFF; });
  if (null_guid || windows_placeholder) {
    return {};
  }
  return GuidString(container);
}

std::wstring GetDeviceId(DEVINST device) {
  ULONG character_count = 0;
  if (CM_Get_Device_ID_Size(&character_count, device, 0) != CR_SUCCESS) {
    return {};
  }
  std::vector<wchar_t> value(static_cast<std::size_t>(character_count) + 1U);
  if (CM_Get_Device_IDW(device, value.data(),
                       static_cast<ULONG>(value.size()), 0) != CR_SUCCESS) {
    return {};
  }
  return value.data();
}

std::vector<std::wstring> GetTopology(DEVINST device) {
  std::vector<std::wstring> topology;
  DEVINST current = device;
  for (int depth = 0; depth < 16; ++depth) {
    std::wstring id = GetDeviceId(current);
    if (!id.empty()) {
      topology.push_back(std::move(id));
    }
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS) {
      break;
    }
    current = parent;
  }
  return topology;
}

std::wstring JoinTopology(const std::vector<std::wstring>& topology) {
  std::wstring result;
  for (const std::wstring& item : topology) {
    if (!result.empty()) {
      result.append(L" <- ");
    }
    result.append(item);
  }
  return result;
}

BusKind ClassifyBus(const std::vector<std::wstring>& topology) {
  for (const std::wstring& id : topology) {
    if (ContainsInsensitive(id, L"BTHENUM\\") ||
        ContainsInsensitive(id, L"BTHLEDEVICE\\") ||
        ContainsInsensitive(id, L"BLUETOOTH")) {
      return BusKind::kBluetooth;
    }
  }
  for (const std::wstring& id : topology) {
    if (ContainsInsensitive(id, L"USB\\")) {
      return BusKind::kUsb;
    }
  }
  for (const std::wstring& id : topology) {
    if (ContainsInsensitive(id, L"I2C\\")) {
      return BusKind::kI2c;
    }
  }
  for (const std::wstring& id : topology) {
    if (ContainsInsensitive(id, L"ACPI\\")) {
      return BusKind::kAcpi;
    }
  }
  return BusKind::kUnknown;
}

std::wstring InstanceIdFromRawPath(std::wstring_view raw_path) {
  constexpr std::wstring_view kPrefix = L"\\\\?\\";
  if (!raw_path.starts_with(kPrefix)) {
    return {};
  }
  const std::size_t class_separator = raw_path.rfind(L"#{");
  if (class_separator == std::wstring_view::npos ||
      class_separator <= kPrefix.size()) {
    return {};
  }
  std::wstring instance(raw_path.substr(
      kPrefix.size(), class_separator - kPrefix.size()));
  std::ranges::replace(instance, L'#', L'\\');
  return instance;
}

bool PopulateFromDeviceNode(std::wstring_view raw_path,
                            DeviceIdentity* identity) {
  identity->instance_id = InstanceIdFromRawPath(raw_path);
  if (identity->instance_id.empty()) {
    return false;
  }

  DEVINST device_instance = 0;
  std::vector<wchar_t> mutable_instance(identity->instance_id.begin(),
                                        identity->instance_id.end());
  mutable_instance.push_back(L'\0');
  if (CM_Locate_DevNodeW(&device_instance, mutable_instance.data(),
                         CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
    return false;
  }

  HDEVINFO devices = SetupDiGetClassDevsW(
      nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
  if (devices == INVALID_HANDLE_VALUE) {
    return false;
  }

  SP_DEVINFO_DATA device_data{};
  device_data.cbSize = sizeof(device_data);
  const bool found = SetupDiOpenDeviceInfoW(
      devices, identity->instance_id.c_str(), nullptr, 0, &device_data) != FALSE;
  if (found) {
    identity->container_id = GetContainerId(devices, &device_data);
    identity->display_name = GetDevicePropertyString(
        devices, &device_data, DEVPKEY_Device_BusReportedDeviceDesc);
    if (identity->display_name.empty()) {
      identity->display_name =
          GetRegistryString(devices, &device_data, SPDRP_FRIENDLYNAME);
    }
    if (identity->display_name.empty()) {
      identity->display_name =
          GetRegistryString(devices, &device_data, SPDRP_DEVICEDESC);
    }
    const std::vector<std::wstring> topology =
        GetTopology(device_data.DevInst);
    identity->topology = JoinTopology(topology);
    identity->bus = ClassifyBus(topology);
  }

  SetupDiDestroyDeviceInfoList(devices);
  return found;
}

}  // namespace

DeviceKey StableKey(std::wstring_view value) {
  constexpr DeviceKey kOffsetBasis = 14695981039346656037ULL;
  constexpr DeviceKey kPrime = 1099511628211ULL;
  DeviceKey hash = kOffsetBasis;
  for (const wchar_t character : value) {
    const wchar_t lowered = static_cast<wchar_t>(std::towlower(character));
    const auto code = static_cast<std::uint32_t>(lowered);
    for (unsigned int shift = 0; shift < 32; shift += 8) {
      hash ^= static_cast<std::uint8_t>((code >> shift) & 0xFFU);
      hash *= kPrime;
    }
  }
  return hash;
}

DeviceIdentity ResolveDeviceIdentity(std::wstring_view raw_path) {
  DeviceIdentity identity;
  identity.raw_path = raw_path;
  identity.device_key = StableKey(raw_path);
  PopulateFromDeviceNode(raw_path, &identity);
  if (identity.display_name.empty()) {
    identity.display_name = L"Raw Input device";
  }
  identity.group_key = identity.container_id.empty()
                           ? identity.device_key
                           : StableKey(L"container:" + identity.container_id);
  return identity;
}

std::wstring_view BusKindName(BusKind bus) {
  switch (bus) {
    case BusKind::kUsb:
      return L"USB";
    case BusKind::kBluetooth:
      return L"Bluetooth";
    case BusKind::kAcpi:
      return L"ACPI/internal";
    case BusKind::kI2c:
      return L"I2C/internal";
    case BusKind::kUnknown:
      return L"unknown";
  }
  return L"unknown";
}

bool IsDefaultRoutableBus(BusKind bus) {
  return bus == BusKind::kUsb;
}

}  // namespace streaming::experiments::usb_routing
