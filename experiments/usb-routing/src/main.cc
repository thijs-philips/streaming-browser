#include "experiments/usb-routing/src/raw_input_capture.h"
#include "experiments/usb-routing/src/websocket_client.h"
#include "src/input_routing/event_queue.h"

#include <windows.h>

#include <conio.h>
#include <fcntl.h>
#include <io.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace usb = streaming::experiments::usb_routing;
namespace {

std::mutex g_console_mutex;

void PrintLine(std::wstring_view message = {}) {
  std::lock_guard lock(g_console_mutex);
  std::wcout << message << L'\n' << std::flush;
}

void PrintHelp() {
  std::lock_guard lock(g_console_mutex);
  std::wcout
      << L"\nCommands (the probe observes but never suppresses Windows input):\n"
      << L"  K  Route a USB keyboard: then press F8 on that keyboard\n"
      << L"  M  Route a USB mouse: then press its middle button\n"
      << L"  L  List detected devices, topology, routes and event counts\n"
      << L"  X  Revoke every route and clear held state\n"
      << L"  Esc  Cancel an armed binding\n"
      << L"  H  Show this help\n"
      << L"  Q  Quit\n\n"
      << L"EMERGENCY STOP / DISABLE ROUTING: Ctrl+Alt+Shift+F12\n"
      << L"Press every key on the same routed USB keyboard. This stops the\n"
      << L"routing agent and returns control to viewer input. The chord is\n"
      << L"ignored on the built-in and other unselected keyboards.\n"
      << std::flush;
}

void PrintDevices(const usb::RawInputCapture& capture) {
  const std::vector<usb::DeviceSnapshot> devices = capture.Devices();
  std::lock_guard lock(g_console_mutex);
  std::wcout << L"\nDetected Raw Input device nodes: " << devices.size()
             << L"\n";
  if (devices.empty()) {
    std::wcout << L"  (none)\n";
  }
  for (const usb::DeviceSnapshot& device : devices) {
    std::wcout << L"\n  " << (device.routed ? L"[ROUTED] " : L"[ignored] ")
               << usb::DeviceKindName(device.kind) << L" — "
               << device.identity.display_name << L"\n"
               << L"    bus: " << usb::BusKindName(device.identity.bus)
               << L"\n"
               << L"    device key: 0x" << std::hex << std::setw(16)
               << std::setfill(L'0') << device.identity.device_key
               << L"  physical group: 0x" << std::setw(16)
               << device.identity.group_key << std::dec << std::setfill(L' ')
               << L"\n"
               << L"    routed packets: " << device.routed_event_count
               << L"\n";
    if (!device.identity.container_id.empty()) {
      std::wcout << L"    container: " << device.identity.container_id
                 << L"\n";
    }
    if (!device.identity.instance_id.empty()) {
      std::wcout << L"    instance: " << device.identity.instance_id << L"\n";
    }
    std::wcout << L"    path: " << device.identity.raw_path << L"\n";
    if (!device.identity.topology.empty()) {
      std::wcout << L"    topology: " << device.identity.topology << L"\n";
    }
  }
  std::wcout
      << L"\nA built-in keyboard can also be on USB. Bus type is diagnostic only;\n"
      << L"routing always requires activation on the intended physical device.\n"
      << std::flush;
}

int RunSynthetic() {
  usb::WebSocketClient websocket;
  std::wstring error;
  if (!websocket.Start({},
                       [](std::wstring message) { PrintLine(message); },
                       &error)) {
    PrintLine(L"Could not start WebSocket client: " + error);
    return 1;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (!websocket.connected() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!websocket.connected()) {
    websocket.Stop();
    PrintLine(L"Synthetic sender could not connect.");
    return 2;
  }
  websocket.ClaimRoute(1);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const std::uint64_t now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  std::vector<streaming::input_routing::RoutedEvent> events;
  events.push_back({streaming::input_routing::EventKind::kKeyDown, 0, 1,
                    now, 'A', 1 | (0x1E << 16), 0, 0});
  events.push_back({streaming::input_routing::EventKind::kKeyUp, 0, 1,
                    now + 1, 'A',
                    static_cast<std::int32_t>(0xC0000001U | (0x1E << 16)),
                    0, 0});
  events.push_back({streaming::input_routing::EventKind::kMouseMove, 0, 2,
                    now + 2, 200, 100, 0, 0});
  events.push_back({streaming::input_routing::EventKind::kMouseButtonDown, 0,
                    2, now + 3, 0, 0, 1, 0});
  events.push_back({streaming::input_routing::EventKind::kMouseButtonUp, 0,
                    2, now + 4, 0, 0, 0, 0});
  if (!websocket.SendEvents(events)) {
    websocket.Stop();
    return 3;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  websocket.ReleaseAll();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  websocket.Stop();
  PrintLine(L"Synthetic input route completed.");
  return 0;
}

}  // namespace

int wmain(int argument_count, wchar_t** arguments) {
  _setmode(_fileno(stdout), _O_U16TEXT);
  _setmode(_fileno(stderr), _O_U16TEXT);
  if (argument_count == 2 &&
      std::wstring_view(arguments[1]) == L"--synthetic") {
    return RunSynthetic();
  }
  std::wcout << L"Streaming Browser USB routing — Raw Input probe\n"
             << L"================================================\n"
             << L"Raw Input registers keyboard/mouse classes globally for this\n"
             << L"process so it can identify the source. Unrouted device packets\n"
             << L"are immediately discarded and key values are never logged.\n";
  PrintHelp();

  std::atomic_bool emergency_stop = false;
  streaming::input_routing::EventQueue event_queue;
  usb::WebSocketClient websocket;
  std::wstring websocket_error;
  if (!websocket.Start({},
                       [](std::wstring message) { PrintLine(L"\n" + message); },
                       &websocket_error)) {
    PrintLine(L"Could not start WebSocket client: " + websocket_error);
    return 1;
  }
  std::atomic_bool sender_stop = false;
  std::thread sender([&] {
    while (!sender_stop.load(std::memory_order_acquire)) {
      const std::vector<streaming::input_routing::RoutedEvent> batch =
          event_queue.Take(streaming::input_routing::kMaxEventsPerBatch);
      if (!batch.empty()) websocket.SendEvents(batch);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  usb::RawInputCapture capture;
  std::wstring error;
  if (!capture.Start(
          [](std::wstring message) { PrintLine(L"\n" + message); },
          [&emergency_stop] {
            emergency_stop.store(true, std::memory_order_release);
          },
          [&event_queue, &websocket](
              streaming::input_routing::RoutedEvent event) {
            if (!event_queue.Push(std::move(event))) {
              websocket.ReleaseAll();
            }
          },
          [&websocket](bool active) {
            if (active) {
              websocket.ClaimRoute(1);
            } else {
              websocket.ReleaseAll();
            }
          },
          &error)) {
    PrintLine(L"Could not start Raw Input capture: " + error);
    sender_stop.store(true, std::memory_order_release);
    sender.join();
    websocket.Stop();
    return 1;
  }

  PrintDevices(capture);
  while (!emergency_stop.load(std::memory_order_acquire)) {
    if (_kbhit() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    const wchar_t command = static_cast<wchar_t>(_getwch());
    switch (std::towupper(command)) {
      case L'K':
        capture.ArmKeyboardBinding();
        PrintLine(L"Keyboard binding armed: press F8 on the USB keyboard. Internal, Bluetooth and unknown-bus devices are refused.");
        break;
      case L'M':
        capture.ArmMouseBinding();
        PrintLine(L"Mouse binding armed: press the USB mouse's middle button. Bluetooth, internal and unknown-bus devices are refused.");
        break;
      case L'L':
        PrintDevices(capture);
        break;
      case L'X':
        event_queue.Clear();
        capture.UnrouteAll();
        PrintLine(L"All routes and held input state were cleared.");
        break;
      case L'H':
        PrintHelp();
        break;
      case L'Q':
        capture.Stop();
        sender_stop.store(true, std::memory_order_release);
        sender.join();
        event_queue.Clear();
        websocket.ReleaseAll();
        websocket.Stop();
        PrintLine(L"USB routing probe stopped.");
        return 0;
      case 27:
        capture.CancelBinding();
        PrintLine(L"Pending device binding cancelled.");
        break;
      default:
        break;
    }
  }

  capture.Stop();
  sender_stop.store(true, std::memory_order_release);
  sender.join();
  event_queue.Clear();
  websocket.ReleaseAll();
  websocket.Stop();
  PrintLine(L"USB routing disabled by Ctrl+Alt+Shift+F12 on the routed keyboard.");
  return 0;
}
