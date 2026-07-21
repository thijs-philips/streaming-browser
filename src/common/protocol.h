#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace streaming::protocol {

inline constexpr std::uint32_t kMagic = 0x53425257;  // "SB RW"
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::size_t kWireHeaderSize = 48;
inline constexpr std::uint32_t kMaxPayloadSize = 1024U * 1024U;
inline constexpr std::uint32_t kViewportWidth = 3840;
inline constexpr std::uint32_t kViewportHeight = 2160;
inline constexpr std::uint32_t kMaxDamageRects = 32;
inline constexpr std::uint32_t kMaxRingSlots = 8;

using SessionId = std::array<std::byte, 16>;

enum class MessageType : std::uint16_t {
  kHello = 1,
  kAccept,
  kReject,
  kRingDefinition,
  kGenerationAccepted,
  kGenerationDrained,
  kFrameReady,
  kFrameReleased,
  kNavigationState,
  kNavigate,
  kBack,
  kForward,
  kReload,
  kStopLoad,
  kCommandResult,
  kInputEvent,
  kImeEvent,
  kCursorState,
  kShowViewer,
  kHideViewer,
  kPing,
  kPong,
  kStreamReset,
  kError,
  kShutdown,
};

struct MessageHeader {
  std::uint16_t major = kProtocolMajor;
  std::uint16_t minor = kProtocolMinor;
  MessageType type = MessageType::kError;
  std::uint16_t flags = 0;
  std::uint32_t payload_size = 0;
  std::uint64_t sequence = 0;
  std::uint64_t generation = 0;
  SessionId session{};
};

struct Rect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
};

struct FrameMetadata {
  std::uint64_t frame_id = 0;
  std::uint64_t producer_timestamp_us = 0;
  std::uint64_t cef_view_timestamp_us = 0;
  std::uint64_t cef_view_counter = 0;
  std::uint32_t slot = 0;
  std::uint32_t width = kViewportWidth;
  std::uint32_t height = kViewportHeight;
  std::uint32_t dropped_unpublished = 0;
  std::uint32_t flags = 0;
  std::vector<Rect> damage;
};

struct RingSlotDefinition {
  std::uint64_t handle = 0;
};

struct RingDefinition {
  std::uint32_t producer_process_id = 0;
  std::uint32_t adapter_luid_low = 0;
  std::int32_t adapter_luid_high = 0;
  std::uint32_t width = kViewportWidth;
  std::uint32_t height = kViewportHeight;
  std::uint32_t dxgi_format = 0;
  std::uint32_t alpha_mode = 1;  // Premultiplied.
  std::vector<RingSlotDefinition> slots;
};

struct FrameRelease {
  std::uint64_t frame_id = 0;
  std::uint32_t slot = 0;
};

enum class InputKind : std::uint16_t {
  kMouseMove = 1,
  kMouseLeave,
  kMouseDown,
  kMouseUp,
  kMouseWheel,
  kKeyDown,
  kKeyUp,
  kCharacter,
  kFocus,
  kCaptureLost,
};

struct InputEvent {
  InputKind kind = InputKind::kMouseMove;
  std::uint16_t modifiers = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t value1 = 0;
  std::int32_t value2 = 0;
};

enum class ImeKind : std::uint16_t {
  kComposition = 1,
  kCommit,
  kFinish,
  kCancel,
};

struct ImeEvent {
  ImeKind kind = ImeKind::kCancel;
  std::u16string text;
};

class ByteWriter final {
 public:
  void WriteU16(std::uint16_t value);
  void WriteU32(std::uint32_t value);
  void WriteU64(std::uint64_t value);
  void WriteI32(std::int32_t value);
  void WriteBytes(std::span<const std::byte> bytes);
  void WriteUtf8(const std::string& value);

  [[nodiscard]] const std::vector<std::byte>& bytes() const { return bytes_; }
  [[nodiscard]] std::vector<std::byte> Take() { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

class ByteReader final {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool ReadU16(std::uint16_t* value);
  bool ReadU32(std::uint32_t* value);
  bool ReadU64(std::uint64_t* value);
  bool ReadI32(std::int32_t* value);
  bool ReadBytes(std::span<std::byte> output);
  bool ReadUtf8(std::string* value, std::uint32_t max_length);

  [[nodiscard]] bool empty() const { return position_ == bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const { return bytes_.size() - position_; }

 private:
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
};

std::array<std::byte, kWireHeaderSize> SerializeHeader(
    const MessageHeader& header);
bool ParseHeader(std::span<const std::byte> bytes,
                 MessageHeader* header,
                 std::string* error);

std::vector<std::byte> SerializeFrameMetadata(const FrameMetadata& metadata);
bool ParseFrameMetadata(std::span<const std::byte> bytes,
                        FrameMetadata* metadata,
                        std::string* error);

std::vector<std::byte> SerializeRingDefinition(
  const RingDefinition& definition);
bool ParseRingDefinition(std::span<const std::byte> bytes,
             RingDefinition* definition,
             std::string* error);

std::vector<std::byte> SerializeFrameRelease(const FrameRelease& release);
bool ParseFrameRelease(std::span<const std::byte> bytes,
             FrameRelease* release,
             std::string* error);

std::vector<std::byte> SerializeInputEvent(const InputEvent& event);
bool ParseInputEvent(std::span<const std::byte> bytes,
           InputEvent* event,
           std::string* error);

std::vector<std::byte> SerializeImeEvent(const ImeEvent& event);
bool ParseImeEvent(std::span<const std::byte> bytes,
                   ImeEvent* event,
                   std::string* error);

bool IsCritical(MessageType type);

}  // namespace streaming::protocol
