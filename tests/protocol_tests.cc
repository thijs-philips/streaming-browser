#include "src/common/protocol.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <string>

namespace {

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using namespace streaming::protocol;

  MessageHeader header;
  header.type = MessageType::kFrameReady;
  header.payload_size = 72;
  header.sequence = 42;
  header.generation = 7;
  header.session[0] = std::byte{0xA5};

  const auto wire = SerializeHeader(header);
  MessageHeader parsed;
  std::string error;
  if (!ParseHeader(wire, &parsed, &error)) {
    return Fail(error.c_str());
  }
  if (parsed.type != header.type || parsed.payload_size != 72 ||
      parsed.sequence != 42 || parsed.generation != 7 ||
      parsed.session[0] != std::byte{0xA5}) {
    return Fail("header round-trip mismatch");
  }

  auto bad_magic = wire;
  bad_magic[0] = std::byte{0};
  if (ParseHeader(bad_magic, &parsed, &error)) {
    return Fail("bad protocol magic was accepted");
  }

  FrameMetadata metadata;
  metadata.frame_id = 99;
  metadata.producer_timestamp_us = 123456;
  metadata.cef_view_timestamp_us = 1000;
  metadata.cef_view_counter = 12;
  metadata.slot = 3;
  metadata.damage.push_back(Rect{10, 20, 30, 40});

  const auto payload = SerializeFrameMetadata(metadata);
  FrameMetadata parsed_metadata;
  if (!ParseFrameMetadata(payload, &parsed_metadata, &error)) {
    return Fail(error.c_str());
  }
  if (parsed_metadata.frame_id != metadata.frame_id ||
      parsed_metadata.slot != metadata.slot ||
      parsed_metadata.damage.size() != 1 ||
      parsed_metadata.damage[0].width != 30) {
    return Fail("frame metadata round-trip mismatch");
  }

  if (!IsCritical(MessageType::kFrameReady) ||
      IsCritical(MessageType::kNavigationState)) {
    return Fail("critical message classification mismatch");
  }

  std::cout << "protocol tests passed\n";
  return 0;
}
