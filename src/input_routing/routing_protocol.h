#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace streaming::input_routing {

inline constexpr std::uint32_t kMagic = 0x53424952;  // "SBIR"
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::size_t kSessionIdSize = 16;
inline constexpr std::size_t kHeaderSize = 48;
inline constexpr std::size_t kMaxEventsPerBatch = 256;
inline constexpr std::size_t kMaxMessageSize = 16 * 1024;

using SessionId = std::array<std::byte, kSessionIdSize>;
using DeviceKey = std::uint64_t;
using RouteId = std::uint64_t;

enum class MessageType : std::uint16_t {
  kHello = 1,
  kAccept,
  kReject,
  kClaimRoute,
  kRouteStatus,
  kEventBatch,
  kStateSnapshot,
  kHeartbeat,
  kReleaseAll,
  kError,
};

enum class EventKind : std::uint16_t {
  kKeyDown = 1,
  kKeyUp,
  kMouseMove,
  kMouseButtonDown,
  kMouseButtonUp,
  kMouseWheel,
};

enum EventFlags : std::uint16_t {
  kEventFlagNone = 0,
  kEventFlagExtended0 = 1U << 0,
  kEventFlagExtended1 = 1U << 1,
  kEventFlagRepeat = 1U << 2,
};

struct MessageHeader {
  std::uint16_t major = kProtocolMajor;
  std::uint16_t minor = kProtocolMinor;
  MessageType type = MessageType::kError;
  std::uint16_t flags = 0;
  std::uint32_t payload_size = 0;
  std::uint64_t sequence = 0;
  RouteId route_id = 0;
  SessionId session{};
};

struct RoutedEvent {
  EventKind kind = EventKind::kMouseMove;
  std::uint16_t flags = 0;
  DeviceKey device_key = 0;
  std::uint64_t timestamp_us = 0;
  std::int32_t value1 = 0;
  std::int32_t value2 = 0;
  std::uint32_t buttons = 0;
  std::uint32_t modifiers = 0;
};

struct StateSnapshot {
  std::vector<std::uint32_t> held_keys;
  std::uint32_t mouse_buttons = 0;
  std::int32_t cursor_x = 0;
  std::int32_t cursor_y = 0;
};

struct Message {
  MessageHeader header;
  std::vector<std::byte> payload;
};

SessionId GenerateSessionId();
std::array<std::byte, kHeaderSize> SerializeHeader(const MessageHeader& header);
bool ParseHeader(std::span<const std::byte> bytes,
                 MessageHeader* header,
                 std::string* error);

std::vector<std::byte> SerializeEvents(std::span<const RoutedEvent> events);
bool ParseEvents(std::span<const std::byte> bytes,
                 std::vector<RoutedEvent>* events,
                 std::string* error);

std::vector<std::byte> SerializeStateSnapshot(const StateSnapshot& snapshot);
bool ParseStateSnapshot(std::span<const std::byte> bytes,
                        StateSnapshot* snapshot,
                        std::string* error);

std::vector<std::byte> SerializeMessage(const Message& message);
bool ParseMessage(std::span<const std::byte> bytes,
                  Message* message,
                  std::string* error);

}  // namespace streaming::input_routing
