#include "src/common/pipe_io.h"

#include <algorithm>
#include <array>
#include <limits>

#include "src/common/win_handle.h"

namespace streaming {
namespace {

void SetError(std::wstring* error, const wchar_t* message) {
  if (error != nullptr) {
    *error = message;
  }
}

}  // namespace

bool ReadExact(HANDLE pipe, std::span<std::byte> output, std::wstring* error) {
  std::size_t position = 0;
  while (position < output.size()) {
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        output.size() - position, std::numeric_limits<DWORD>::max()));
    DWORD received = 0;
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
      SetError(error, L"named-pipe read event creation failed");
      return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (!ReadFile(pipe, output.data() + position, request, nullptr,
                  &operation)) {
      if (GetLastError() != ERROR_IO_PENDING ||
          WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0 ||
          !GetOverlappedResult(pipe, &operation, &received, FALSE)) {
        SetError(error, L"named-pipe read failed");
        return false;
      }
    } else if (!GetOverlappedResult(pipe, &operation, &received, TRUE)) {
      SetError(error, L"named-pipe read completion failed");
      return false;
    }
    if (received == 0) {
      SetError(error, L"named pipe closed during read");
      return false;
    }
    position += received;
  }
  return true;
}

bool WriteExact(HANDLE pipe,
                std::span<const std::byte> input,
                std::wstring* error) {
  std::size_t position = 0;
  while (position < input.size()) {
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
        input.size() - position, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
      SetError(error, L"named-pipe write event creation failed");
      return false;
    }
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (!WriteFile(pipe, input.data() + position, request, nullptr,
                   &operation)) {
      if (GetLastError() != ERROR_IO_PENDING ||
          WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0 ||
          !GetOverlappedResult(pipe, &operation, &written, FALSE)) {
        SetError(error, L"named-pipe write failed");
        return false;
      }
    } else if (!GetOverlappedResult(pipe, &operation, &written, TRUE)) {
      SetError(error, L"named-pipe write completion failed");
      return false;
    }
    if (written == 0) {
      SetError(error, L"named pipe closed during write");
      return false;
    }
    position += written;
  }
  return true;
}

bool ReadMessage(HANDLE pipe, ReceivedMessage* message, std::wstring* error) {
  if (message == nullptr) {
    SetError(error, L"null message output");
    return false;
  }

  std::array<std::byte, protocol::kWireHeaderSize> wire_header{};
  if (!ReadExact(pipe, wire_header, error)) {
    return false;
  }
  std::string parse_error;
  if (!protocol::ParseHeader(wire_header, &message->header, &parse_error)) {
    if (error != nullptr) {
      error->assign(parse_error.begin(), parse_error.end());
    }
    return false;
  }

  message->payload.resize(message->header.payload_size);
  return ReadExact(pipe, message->payload, error);
}

bool WriteMessage(HANDLE pipe,
                  protocol::MessageHeader header,
                  std::span<const std::byte> payload,
                  std::wstring* error) {
  if (payload.size() > protocol::kMaxPayloadSize) {
    SetError(error, L"message payload exceeds protocol limit");
    return false;
  }
  header.payload_size = static_cast<std::uint32_t>(payload.size());
  const auto wire_header = protocol::SerializeHeader(header);
  return WriteExact(pipe, wire_header, error) &&
         WriteExact(pipe, payload, error);
}

}  // namespace streaming
