#include "src/input_routing/routing_protocol.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace streaming::input_routing {
namespace {

void SetError(std::string* error, std::string value) {
  if (error != nullptr) {
    *error = std::move(value);
  }
}

class Writer final {
 public:
  void U16(std::uint16_t value) {
    bytes_.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
  }
  void U32(std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
      bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
  }
  void U64(std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
      bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
  }
  void I32(std::int32_t value) { U32(static_cast<std::uint32_t>(value)); }
  void Bytes(std::span<const std::byte> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }
  std::vector<std::byte> Take() { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

class Reader final {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool U16(std::uint16_t* value) {
    if (remaining() < 2) return false;
    *value = static_cast<std::uint16_t>(bytes_[position_]) |
             static_cast<std::uint16_t>(bytes_[position_ + 1]) << 8U;
    position_ += 2;
    return true;
  }
  bool U32(std::uint32_t* value) {
    if (remaining() < 4) return false;
    *value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
      *value |= static_cast<std::uint32_t>(bytes_[position_++]) << shift;
    }
    return true;
  }
  bool U64(std::uint64_t* value) {
    if (remaining() < 8) return false;
    *value = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
      *value |= static_cast<std::uint64_t>(bytes_[position_++]) << shift;
    }
    return true;
  }
  bool I32(std::int32_t* value) {
    std::uint32_t raw = 0;
    if (!U32(&raw)) return false;
    *value = static_cast<std::int32_t>(raw);
    return true;
  }
  bool Bytes(std::span<std::byte> output) {
    if (remaining() < output.size()) return false;
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                output.size(), output.begin());
    position_ += output.size();
    return true;
  }
  [[nodiscard]] std::size_t remaining() const {
    return bytes_.size() - position_;
  }

 private:
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
};

bool IsKnownType(std::uint16_t type) {
  return type >= static_cast<std::uint16_t>(MessageType::kHello) &&
         type <= static_cast<std::uint16_t>(MessageType::kError);
}

bool IsKnownEvent(std::uint16_t kind) {
  return kind >= static_cast<std::uint16_t>(EventKind::kKeyDown) &&
         kind <= static_cast<std::uint16_t>(EventKind::kMouseWheel);
}

}  // namespace

SessionId GenerateSessionId() {
  SessionId result{};
  const NTSTATUS status = BCryptGenRandom(
      nullptr, reinterpret_cast<PUCHAR>(result.data()),
      static_cast<ULONG>(result.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::memcpy(result.data(), &counter,
                std::min(result.size(), sizeof(counter)));
  }
  return result;
}

std::array<std::byte, kHeaderSize> SerializeHeader(const MessageHeader& header) {
  Writer writer;
  writer.U32(kMagic);
  writer.U16(header.major);
  writer.U16(header.minor);
  writer.U16(static_cast<std::uint16_t>(header.type));
  writer.U16(header.flags);
  writer.U32(header.payload_size);
  writer.U64(header.sequence);
  writer.U64(header.route_id);
  writer.Bytes(header.session);
  const std::vector<std::byte> bytes = writer.Take();
  std::array<std::byte, kHeaderSize> result{};
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}

bool ParseHeader(std::span<const std::byte> bytes,
                 MessageHeader* header,
                 std::string* error) {
  if (header == nullptr || bytes.size() != kHeaderSize) {
    SetError(error, "invalid input-routing header size");
    return false;
  }
  Reader reader(bytes);
  std::uint32_t magic = 0;
  std::uint16_t type = 0;
  if (!reader.U32(&magic) || !reader.U16(&header->major) ||
      !reader.U16(&header->minor) || !reader.U16(&type) ||
      !reader.U16(&header->flags) || !reader.U32(&header->payload_size) ||
      !reader.U64(&header->sequence) || !reader.U64(&header->route_id) ||
      !reader.Bytes(header->session) || reader.remaining() != 0) {
    SetError(error, "truncated input-routing header");
    return false;
  }
  if (magic != kMagic) {
    SetError(error, "invalid input-routing magic");
    return false;
  }
  if (header->major != kProtocolMajor || header->minor > kProtocolMinor) {
    SetError(error, "unsupported input-routing protocol version");
    return false;
  }
  if (!IsKnownType(type)) {
    SetError(error, "unknown input-routing message type");
    return false;
  }
  if (header->payload_size > kMaxMessageSize - kHeaderSize) {
    SetError(error, "input-routing payload exceeds limit");
    return false;
  }
  header->type = static_cast<MessageType>(type);
  return true;
}

std::vector<std::byte> SerializeEvents(std::span<const RoutedEvent> events) {
  Writer writer;
  const auto count = static_cast<std::uint16_t>(
      std::min(events.size(), kMaxEventsPerBatch));
  writer.U16(count);
  writer.U16(0);
  for (std::uint16_t index = 0; index < count; ++index) {
    const RoutedEvent& event = events[index];
    writer.U16(static_cast<std::uint16_t>(event.kind));
    writer.U16(event.flags);
    writer.U64(event.device_key);
    writer.U64(event.timestamp_us);
    writer.I32(event.value1);
    writer.I32(event.value2);
    writer.U32(event.buttons);
    writer.U32(event.modifiers);
  }
  return writer.Take();
}

bool ParseEvents(std::span<const std::byte> bytes,
                 std::vector<RoutedEvent>* events,
                 std::string* error) {
  if (events == nullptr) {
    SetError(error, "null routed-event output");
    return false;
  }
  Reader reader(bytes);
  std::uint16_t count = 0;
  std::uint16_t reserved = 0;
  if (!reader.U16(&count) || !reader.U16(&reserved) ||
      count > kMaxEventsPerBatch || reserved != 0) {
    SetError(error, "invalid routed-event batch header");
    return false;
  }
  events->clear();
  events->reserve(count);
  for (std::uint16_t index = 0; index < count; ++index) {
    RoutedEvent event;
    std::uint16_t kind = 0;
    if (!reader.U16(&kind) || !reader.U16(&event.flags) ||
        !reader.U64(&event.device_key) || !reader.U64(&event.timestamp_us) ||
        !reader.I32(&event.value1) || !reader.I32(&event.value2) ||
        !reader.U32(&event.buttons) || !reader.U32(&event.modifiers) ||
        !IsKnownEvent(kind)) {
      SetError(error, "invalid routed event");
      events->clear();
      return false;
    }
    event.kind = static_cast<EventKind>(kind);
    events->push_back(event);
  }
  if (reader.remaining() != 0) {
    SetError(error, "unexpected routed-event batch data");
    events->clear();
    return false;
  }
  return true;
}

std::vector<std::byte> SerializeStateSnapshot(const StateSnapshot& snapshot) {
  Writer writer;
  const auto count = static_cast<std::uint16_t>(
      std::min<std::size_t>(snapshot.held_keys.size(), 256));
  writer.U16(count);
  writer.U16(0);
  writer.U32(snapshot.mouse_buttons);
  writer.I32(snapshot.cursor_x);
  writer.I32(snapshot.cursor_y);
  for (std::uint16_t index = 0; index < count; ++index) {
    writer.U32(snapshot.held_keys[index]);
  }
  return writer.Take();
}

bool ParseStateSnapshot(std::span<const std::byte> bytes,
                        StateSnapshot* snapshot,
                        std::string* error) {
  if (snapshot == nullptr) {
    SetError(error, "null state snapshot output");
    return false;
  }
  Reader reader(bytes);
  std::uint16_t count = 0;
  std::uint16_t reserved = 0;
  if (!reader.U16(&count) || !reader.U16(&reserved) || reserved != 0 ||
      count > 256 || !reader.U32(&snapshot->mouse_buttons) ||
      !reader.I32(&snapshot->cursor_x) || !reader.I32(&snapshot->cursor_y)) {
    SetError(error, "invalid state snapshot header");
    return false;
  }
  snapshot->held_keys.clear();
  snapshot->held_keys.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index) {
    std::uint32_t key = 0;
    if (!reader.U32(&key)) {
      SetError(error, "truncated state snapshot");
      return false;
    }
    snapshot->held_keys.push_back(key);
  }
  if (reader.remaining() != 0) {
    SetError(error, "unexpected state snapshot data");
    return false;
  }
  return true;
}

std::vector<std::byte> SerializeMessage(const Message& message) {
  MessageHeader header = message.header;
  header.payload_size = static_cast<std::uint32_t>(message.payload.size());
  if (message.payload.size() > kMaxMessageSize - kHeaderSize) {
    return {};
  }
  const auto wire_header = SerializeHeader(header);
  std::vector<std::byte> result;
  result.reserve(wire_header.size() + message.payload.size());
  result.insert(result.end(), wire_header.begin(), wire_header.end());
  result.insert(result.end(), message.payload.begin(), message.payload.end());
  return result;
}

bool ParseMessage(std::span<const std::byte> bytes,
                  Message* message,
                  std::string* error) {
  if (message == nullptr || bytes.size() < kHeaderSize ||
      bytes.size() > kMaxMessageSize) {
    SetError(error, "invalid input-routing message size");
    return false;
  }
  if (!ParseHeader(bytes.first(kHeaderSize), &message->header, error)) {
    return false;
  }
  if (bytes.size() != kHeaderSize + message->header.payload_size) {
    SetError(error, "input-routing payload size mismatch");
    return false;
  }
  message->payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                          bytes.end());
  return true;
}

}  // namespace streaming::input_routing
