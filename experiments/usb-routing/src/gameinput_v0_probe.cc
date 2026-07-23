#include <windows.h>

#include <GameInput.h>

#include <conio.h>
#include <fcntl.h>
#include <io.h>

#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>

extern "C" HRESULT WINAPI GameInputInitialize(REFIID riid, void** output);

namespace {

struct ProbeState {
  std::mutex mutex;
  std::unordered_map<IGameInputDevice*, std::uint64_t> readings;
};

std::wstring ToWide(const GameInputString* value) {
  if (value == nullptr || value->data == nullptr || value->sizeInBytes == 0)
    return {};
  const int byte_count = static_cast<int>(value->sizeInBytes);
  const int count = MultiByteToWideChar(CP_UTF8, 0, value->data, byte_count,
                                        nullptr, 0);
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value->data, byte_count, result.data(), count);
  while (!result.empty() && result.back() == L'\0') result.pop_back();
  return result;
}

std::wstring DeviceId(const APP_LOCAL_DEVICE_ID& id) {
  std::wostringstream stream;
  stream << std::hex << std::setfill(L'0');
  for (const std::uint8_t value : id.value)
    stream << std::setw(2) << static_cast<unsigned int>(value);
  return stream.str();
}

void CALLBACK DeviceCallback(GameInputCallbackToken,
                             void* context,
                             IGameInputDevice* device,
                             std::uint64_t,
                             GameInputDeviceStatus current,
                             GameInputDeviceStatus) {
  if ((current & GameInputDeviceConnected) == 0) return;
  const GameInputDeviceInfo* info = device->GetDeviceInfo();
  if (info == nullptr) return;
  const std::wstring name = ToWide(info->displayName);
  std::lock_guard lock(static_cast<ProbeState*>(context)->mutex);
  std::wcout << L"\n" << (name.empty() ? L"GameInput device" : name)
             << L"\n  kinds:"
             << ((info->supportedInput & GameInputKindKeyboard) != 0
                     ? L" keyboard" : L"")
             << ((info->supportedInput & GameInputKindMouse) != 0
                     ? L" mouse" : L"")
             << L"\n  VID:PID: " << std::hex << std::setw(4)
             << std::setfill(L'0') << info->vendorId << L":" << std::setw(4)
             << info->productId << std::dec << std::setfill(L' ')
             << L"\n  device ID: " << DeviceId(info->deviceId)
             << L"\n  root ID:   " << DeviceId(info->deviceRootId) << L"\n";
}

void CALLBACK ReadingCallback(GameInputCallbackToken,
                              void* context,
                              IGameInputReading* reading,
                              bool) {
  IGameInputDevice* device = nullptr;
  reading->GetDevice(&device);
  if (device == nullptr) return;
  auto* state = static_cast<ProbeState*>(context);
  {
    std::lock_guard lock(state->mutex);
    ++state->readings[device];
  }
  device->Release();
}

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
  _setmode(_fileno(stdout), _O_U16TEXT);
  _setmode(_fileno(stderr), _O_U16TEXT);
  std::wcout << L"GameInput 3.4.259 package / API v0 comparison probe\n"
             << L"No key values are logged. Press Q to stop and show counts.\n";

  IGameInput* input = nullptr;
  const HRESULT created =
      GameInputInitialize(__uuidof(IGameInput),
                          reinterpret_cast<void**>(&input));
  if (FAILED(created) || input == nullptr) {
    std::wcerr << L"GameInput initialization failed: 0x" << std::hex
               << static_cast<std::uint32_t>(created) << L"\n"
               << L"GameInputRedist.msi needs an elevated installer.\n";
    return 1;
  }

  ProbeState state;
  GameInputCallbackToken device_token = 0;
  GameInputCallbackToken keyboard_token = 0;
  GameInputCallbackToken mouse_token = 0;
  const auto kinds = static_cast<GameInputKind>(
      GameInputKindKeyboard | GameInputKindMouse);
  HRESULT result = input->RegisterDeviceCallback(
      nullptr, kinds, GameInputDeviceAnyStatus, GameInputBlockingEnumeration,
      &state, &DeviceCallback, &device_token);
  if (FAILED(result)) {
    input->Release();
    return 2;
  }
  result = input->RegisterReadingCallback(
      nullptr, GameInputKindKeyboard, 0.0F, &state, &ReadingCallback,
      &keyboard_token);
  if (FAILED(result)) {
    std::wcerr << L"Keyboard reading callback failed: 0x" << std::hex
               << static_cast<std::uint32_t>(result) << L"\n";
    input->UnregisterCallback(device_token, 5000);
    input->Release();
    return 3;
  }
  result = input->RegisterReadingCallback(
      nullptr, GameInputKindMouse, 0.0F, &state, &ReadingCallback,
      &mouse_token);
  if (FAILED(result)) {
    std::wcerr << L"Mouse reading callback failed: 0x" << std::hex
               << static_cast<std::uint32_t>(result) << L"\n";
    input->UnregisterCallback(keyboard_token, 5000);
    input->UnregisterCallback(device_token, 5000);
    input->Release();
    return 4;
  }

  const bool timed = argument_count == 2 &&
                     std::wstring_view(arguments[1]) == L"--timed";
  const ULONGLONG deadline = GetTickCount64() + 1000;
  while (!timed || GetTickCount64() < deadline) {
    if (_kbhit() != 0 && std::towupper(_getwch()) == L'Q') break;
    Sleep(20);
  }

  input->UnregisterCallback(mouse_token, 5000);
  input->UnregisterCallback(keyboard_token, 5000);
  input->UnregisterCallback(device_token, 5000);
  {
    std::lock_guard lock(state.mutex);
    std::wcout << L"\nPer-device reading counts:\n";
    for (const auto& [device, count] : state.readings)
      std::wcout << L"  " << device << L": " << count << L"\n";
  }
  input->Release();
  return 0;
}
