#include "src/common/protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace streaming::protocol {
namespace {

template <typename T>
void WriteLittleEndian(std::vector<std::byte>* output, T value) {
  static_assert(std::is_unsigned_v<T>);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    output->push_back(static_cast<std::byte>((value >> (i * 8U)) & 0xFFU));
  }
}

template <typename T>
bool ReadLittleEndian(std::span<const std::byte> bytes,
                      std::size_t* position,
                      T* value) {
  static_assert(std::is_unsigned_v<T>);
  if (*position > bytes.size() || bytes.size() - *position < sizeof(T)) {
    return false;
  }
  T result = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    result |= static_cast<T>(std::to_integer<unsigned char>(bytes[*position + i]))
              << (i * 8U);
  }
  *position += sizeof(T);
  *value = result;
  return true;
}

void SetError(std::string* error, const char* value) {
  if (error != nullptr) {
    *error = value;
  }
}

}  // namespace

void ByteWriter::WriteU16(std::uint16_t value) {
  WriteLittleEndian(&bytes_, value);
}

void ByteWriter::WriteU32(std::uint32_t value) {
  WriteLittleEndian(&bytes_, value);
}

void ByteWriter::WriteU64(std::uint64_t value) {
  WriteLittleEndian(&bytes_, value);
}

void ByteWriter::WriteI32(std::int32_t value) {
  WriteU32(static_cast<std::uint32_t>(value));
}

void ByteWriter::WriteBytes(std::span<const std::byte> bytes) {
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::WriteUtf8(const std::string& value) {
  WriteU32(static_cast<std::uint32_t>(value.size()));
  WriteBytes(std::as_bytes(std::span(value.data(), value.size())));
}

bool ByteReader::ReadU16(std::uint16_t* value) {
  return value != nullptr && ReadLittleEndian(bytes_, &position_, value);
}

bool ByteReader::ReadU32(std::uint32_t* value) {
  return value != nullptr && ReadLittleEndian(bytes_, &position_, value);
}

bool ByteReader::ReadU64(std::uint64_t* value) {
  return value != nullptr && ReadLittleEndian(bytes_, &position_, value);
}

bool ByteReader::ReadI32(std::int32_t* value) {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t raw = 0;
  if (!ReadU32(&raw)) {
    return false;
  }
  *value = static_cast<std::int32_t>(raw);
  return true;
}

bool ByteReader::ReadBytes(std::span<std::byte> output) {
  if (position_ > bytes_.size() || bytes_.size() - position_ < output.size()) {
    return false;
  }
  std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
              output.size(), output.begin());
  position_ += output.size();
  return true;
}

bool ByteReader::ReadUtf8(std::string* value, std::uint32_t max_length) {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t length = 0;
  if (!ReadU32(&length) || length > max_length || remaining() < length) {
    return false;
  }
  const char* begin = reinterpret_cast<const char*>(bytes_.data() + position_);
  value->assign(begin, begin + length);
  position_ += length;
  return true;
}

std::array<std::byte, kWireHeaderSize> SerializeHeader(
    const MessageHeader& header) {
  ByteWriter writer;
  writer.WriteU32(kMagic);
  writer.WriteU16(header.major);
  writer.WriteU16(header.minor);
  writer.WriteU16(static_cast<std::uint16_t>(header.type));
  writer.WriteU16(header.flags);
  writer.WriteU32(header.payload_size);
  writer.WriteU64(header.sequence);
  writer.WriteU64(header.generation);
  writer.WriteBytes(header.session);

  std::array<std::byte, kWireHeaderSize> result{};
  const auto& bytes = writer.bytes();
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}

bool ParseHeader(std::span<const std::byte> bytes,
                 MessageHeader* header,
                 std::string* error) {
  if (header == nullptr || bytes.size() != kWireHeaderSize) {
    SetError(error, "invalid header size");
    return false;
  }

  ByteReader reader(bytes);
  std::uint32_t magic = 0;
  std::uint16_t type = 0;
  if (!reader.ReadU32(&magic) || !reader.ReadU16(&header->major) ||
      !reader.ReadU16(&header->minor) || !reader.ReadU16(&type) ||
      !reader.ReadU16(&header->flags) ||
      !reader.ReadU32(&header->payload_size) ||
      !reader.ReadU64(&header->sequence) ||
      !reader.ReadU64(&header->generation) ||
      !reader.ReadBytes(header->session)) {
    SetError(error, "truncated header");
    return false;
  }
  header->type = static_cast<MessageType>(type);

  if (magic != kMagic) {
    SetError(error, "invalid protocol magic");
    return false;
  }
  if (header->major != kProtocolMajor) {
    SetError(error, "incompatible protocol major version");
    return false;
  }
  if (header->payload_size > kMaxPayloadSize) {
    SetError(error, "payload exceeds protocol limit");
    return false;
  }
  return true;
}

std::vector<std::byte> SerializeFrameMetadata(const FrameMetadata& metadata) {
  ByteWriter writer;
  writer.WriteU64(metadata.frame_id);
  writer.WriteU64(metadata.producer_timestamp_us);
  writer.WriteU64(metadata.cef_view_timestamp_us);
  writer.WriteU64(metadata.cef_view_counter);
  writer.WriteU32(metadata.slot);
  writer.WriteU32(metadata.width);
  writer.WriteU32(metadata.height);
  writer.WriteU32(metadata.dropped_unpublished);
  writer.WriteU32(metadata.flags);
  const auto count = static_cast<std::uint32_t>(
      std::min<std::size_t>(metadata.damage.size(), kMaxDamageRects));
  writer.WriteU32(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    writer.WriteI32(metadata.damage[i].x);
    writer.WriteI32(metadata.damage[i].y);
    writer.WriteI32(metadata.damage[i].width);
    writer.WriteI32(metadata.damage[i].height);
  }
  return writer.Take();
}

bool ParseFrameMetadata(std::span<const std::byte> bytes,
                        FrameMetadata* metadata,
                        std::string* error) {
  if (metadata == nullptr) {
    SetError(error, "null frame metadata output");
    return false;
  }

  ByteReader reader(bytes);
  std::uint32_t damage_count = 0;
  if (!reader.ReadU64(&metadata->frame_id) ||
      !reader.ReadU64(&metadata->producer_timestamp_us) ||
      !reader.ReadU64(&metadata->cef_view_timestamp_us) ||
      !reader.ReadU64(&metadata->cef_view_counter) ||
      !reader.ReadU32(&metadata->slot) ||
      !reader.ReadU32(&metadata->width) ||
      !reader.ReadU32(&metadata->height) ||
      !reader.ReadU32(&metadata->dropped_unpublished) ||
      !reader.ReadU32(&metadata->flags) ||
      !reader.ReadU32(&damage_count)) {
    SetError(error, "truncated frame metadata");
    return false;
  }
  if (damage_count > kMaxDamageRects) {
    SetError(error, "too many damage rectangles");
    return false;
  }

  metadata->damage.clear();
  metadata->damage.reserve(damage_count);
  for (std::uint32_t i = 0; i < damage_count; ++i) {
    Rect rect;
    if (!reader.ReadI32(&rect.x) || !reader.ReadI32(&rect.y) ||
        !reader.ReadI32(&rect.width) || !reader.ReadI32(&rect.height)) {
      SetError(error, "truncated damage rectangle");
      return false;
    }
    if (rect.width < 0 || rect.height < 0) {
      SetError(error, "negative damage rectangle extent");
      return false;
    }
    metadata->damage.push_back(rect);
  }
  if (!reader.empty()) {
    SetError(error, "unexpected trailing frame metadata");
    return false;
  }
  return true;
}

std::vector<std::byte> SerializeRingDefinition(
    const RingDefinition& definition) {
  ByteWriter writer;
  writer.WriteU32(definition.producer_process_id);
  writer.WriteU32(definition.adapter_luid_low);
  writer.WriteI32(definition.adapter_luid_high);
  writer.WriteU32(definition.width);
  writer.WriteU32(definition.height);
  writer.WriteU32(definition.dxgi_format);
  writer.WriteU32(definition.alpha_mode);
  const auto count = static_cast<std::uint32_t>(
      std::min<std::size_t>(definition.slots.size(), kMaxRingSlots));
  writer.WriteU32(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    writer.WriteU64(definition.slots[i].handle);
  }
  return writer.Take();
}

bool ParseRingDefinition(std::span<const std::byte> bytes,
                         RingDefinition* definition,
                         std::string* error) {
  if (definition == nullptr) {
    SetError(error, "null ring definition output");
    return false;
  }
  ByteReader reader(bytes);
  std::uint32_t slot_count = 0;
  if (!reader.ReadU32(&definition->producer_process_id) ||
      !reader.ReadU32(&definition->adapter_luid_low) ||
      !reader.ReadI32(&definition->adapter_luid_high) ||
      !reader.ReadU32(&definition->width) ||
      !reader.ReadU32(&definition->height) ||
      !reader.ReadU32(&definition->dxgi_format) ||
      !reader.ReadU32(&definition->alpha_mode) ||
      !reader.ReadU32(&slot_count)) {
    SetError(error, "truncated ring definition");
    return false;
  }
  if (slot_count == 0 || slot_count > kMaxRingSlots) {
    SetError(error, "invalid ring slot count");
    return false;
  }
  definition->slots.clear();
  definition->slots.reserve(slot_count);
  for (std::uint32_t i = 0; i < slot_count; ++i) {
    RingSlotDefinition slot;
    if (!reader.ReadU64(&slot.handle) || slot.handle == 0) {
      SetError(error, "invalid ring slot handle");
      return false;
    }
    definition->slots.push_back(slot);
  }
  if (!reader.empty()) {
    SetError(error, "unexpected trailing ring definition data");
    return false;
  }
  return true;
}

std::vector<std::byte> SerializeFrameRelease(const FrameRelease& release) {
  ByteWriter writer;
  writer.WriteU64(release.frame_id);
  writer.WriteU32(release.slot);
  return writer.Take();
}

bool ParseFrameRelease(std::span<const std::byte> bytes,
                       FrameRelease* release,
                       std::string* error) {
  if (release == nullptr) {
    SetError(error, "null frame release output");
    return false;
  }
  ByteReader reader(bytes);
  if (!reader.ReadU64(&release->frame_id) ||
      !reader.ReadU32(&release->slot) || !reader.empty()) {
    SetError(error, "invalid frame release");
    return false;
  }
  return true;
}

std::vector<std::byte> SerializeInputEvent(const InputEvent& event) {
  ByteWriter writer;
  writer.WriteU16(static_cast<std::uint16_t>(event.kind));
  writer.WriteU16(event.modifiers);
  writer.WriteI32(event.x);
  writer.WriteI32(event.y);
  writer.WriteI32(event.value1);
  writer.WriteI32(event.value2);
  return writer.Take();
}

bool ParseInputEvent(std::span<const std::byte> bytes,
                     InputEvent* event,
                     std::string* error) {
  if (event == nullptr) {
    SetError(error, "null input event output");
    return false;
  }
  ByteReader reader(bytes);
  std::uint16_t kind = 0;
  if (!reader.ReadU16(&kind) || !reader.ReadU16(&event->modifiers) ||
      !reader.ReadI32(&event->x) || !reader.ReadI32(&event->y) ||
      !reader.ReadI32(&event->value1) || !reader.ReadI32(&event->value2) ||
      !reader.empty()) {
    SetError(error, "invalid input event");
    return false;
  }
  if (kind < static_cast<std::uint16_t>(InputKind::kMouseMove) ||
      kind > static_cast<std::uint16_t>(InputKind::kCaptureLost)) {
    SetError(error, "unknown input event kind");
    return false;
  }
  event->kind = static_cast<InputKind>(kind);
  return true;
}

std::vector<std::byte> SerializeImeEvent(const ImeEvent& event) {
  ByteWriter writer;
  writer.WriteU16(static_cast<std::uint16_t>(event.kind));
  const auto length = static_cast<std::uint32_t>(
      std::min<std::size_t>(event.text.size(), 4096));
  writer.WriteU32(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    writer.WriteU16(static_cast<std::uint16_t>(event.text[i]));
  }
  return writer.Take();
}

bool ParseImeEvent(std::span<const std::byte> bytes,
                   ImeEvent* event,
                   std::string* error) {
  if (event == nullptr) {
    SetError(error, "null IME event output");
    return false;
  }
  ByteReader reader(bytes);
  std::uint16_t kind = 0;
  std::uint32_t length = 0;
  if (!reader.ReadU16(&kind) || !reader.ReadU32(&length) || length > 4096) {
    SetError(error, "invalid IME event header");
    return false;
  }
  if (kind < static_cast<std::uint16_t>(ImeKind::kComposition) ||
      kind > static_cast<std::uint16_t>(ImeKind::kCancel)) {
    SetError(error, "unknown IME event kind");
    return false;
  }
  event->kind = static_cast<ImeKind>(kind);
  event->text.clear();
  event->text.reserve(length);
  for (std::uint32_t i = 0; i < length; ++i) {
    std::uint16_t character = 0;
    if (!reader.ReadU16(&character)) {
      SetError(error, "truncated IME event text");
      return false;
    }
    event->text.push_back(static_cast<char16_t>(character));
  }
  if (!reader.empty()) {
    SetError(error, "unexpected trailing IME event data");
    return false;
  }
  return true;
}

bool IsCritical(MessageType type) {
  switch (type) {
    case MessageType::kRingDefinition:
    case MessageType::kGenerationAccepted:
    case MessageType::kGenerationDrained:
    case MessageType::kFrameReady:
    case MessageType::kFrameReleased:
    case MessageType::kStreamReset:
    case MessageType::kError:
    case MessageType::kShutdown:
      return true;
    default:
      return false;
  }
}

}  // namespace streaming::protocol
