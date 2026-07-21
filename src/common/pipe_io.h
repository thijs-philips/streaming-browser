#pragma once

#include <windows.h>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "src/common/protocol.h"

namespace streaming {

struct ReceivedMessage {
  protocol::MessageHeader header;
  std::vector<std::byte> payload;
};

bool ReadExact(HANDLE pipe, std::span<std::byte> output, std::wstring* error);
bool WriteExact(HANDLE pipe,
                std::span<const std::byte> input,
                std::wstring* error);
bool ReadMessage(HANDLE pipe, ReceivedMessage* message, std::wstring* error);
bool WriteMessage(HANDLE pipe,
                  protocol::MessageHeader header,
                  std::span<const std::byte> payload,
                  std::wstring* error);

}  // namespace streaming
