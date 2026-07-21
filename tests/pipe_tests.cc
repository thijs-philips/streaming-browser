#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "src/common/pipe_io.h"
#include "src/common/protocol.h"
#include "src/common/win_handle.h"

namespace {

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using namespace streaming;
  using namespace streaming::protocol;

  const std::wstring pipe_name =
      L"\\\\.\\pipe\\StreamingBrowser.PipeTest." +
      std::to_wstring(GetCurrentProcessId());
  UniqueHandle server(CreateNamedPipeW(
      pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 1000,
      nullptr));
  if (!server) return Fail("failed to create test pipe");

  std::atomic_bool server_ok = false;
  std::thread server_thread([&] {
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    bool connected = ConnectNamedPipe(server.get(), &operation) != FALSE;
    if (!connected && GetLastError() == ERROR_IO_PENDING) {
      connected = WaitForSingleObject(event.get(), 5000) == WAIT_OBJECT_0;
    }
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) return;

    ReceivedMessage received;
    std::wstring error;
    if (!ReadMessage(server.get(), &received, &error) ||
        received.header.type != MessageType::kPing) {
      return;
    }
    MessageHeader pong;
    pong.type = MessageType::kPong;
    pong.sequence = 2;
    server_ok = WriteMessage(server.get(), pong, received.payload, &error);
  });

  UniqueHandle client(CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr));
  if (!client) {
    server_thread.join();
    return Fail("failed to open test pipe");
  }

  MessageHeader ping;
  ping.type = MessageType::kPing;
  ping.sequence = 1;
  std::vector<std::byte> payload(256 * 1024);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>(i & 0xFFU);
  }
  std::wstring error;
  if (!WriteMessage(client.get(), ping, payload, &error)) {
    server_thread.join();
    return Fail("failed to write test message");
  }
  ReceivedMessage pong;
  if (!ReadMessage(client.get(), &pong, &error) ||
      pong.header.type != MessageType::kPong || pong.payload != payload) {
    server_thread.join();
    return Fail("pipe message round-trip mismatch");
  }
  server_thread.join();
  if (!server_ok) return Fail("server failed to write pong");

  std::cout << "pipe tests passed\n";
  return 0;
}
