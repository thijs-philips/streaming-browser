#include "src/input_routing/event_queue.h"
#include "src/input_routing/route_state.h"
#include "src/input_routing/routing_protocol.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace routing = streaming::input_routing;
namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
  }
}

void TestProtocolRoundTrip() {
  routing::Message message;
  message.header.type = routing::MessageType::kEventBatch;
  message.header.sequence = 42;
  message.header.route_id = 7;
  message.header.session[0] = std::byte{0xA5};
  routing::RoutedEvent event;
  event.kind = routing::EventKind::kMouseMove;
  event.device_key = 99;
  event.timestamp_us = 1234;
  event.value1 = -7;
  event.value2 = 11;
  message.payload = routing::SerializeEvents({&event, 1});

  const std::vector<std::byte> wire = routing::SerializeMessage(message);
  routing::Message parsed;
  std::string error;
  Check(routing::ParseMessage(wire, &parsed, &error),
        "message parses after serialization");
  Check(parsed.header.sequence == 42 && parsed.header.route_id == 7 &&
            parsed.header.session[0] == std::byte{0xA5},
        "message header round trip");
  std::vector<routing::RoutedEvent> events;
  Check(routing::ParseEvents(parsed.payload, &events, &error),
        "event batch parses");
  Check(events.size() == 1 && events[0].device_key == 99 &&
            events[0].value1 == -7 && events[0].value2 == 11,
        "event batch round trip");

  auto bad_magic = wire;
  bad_magic[0] = std::byte{0};
  Check(!routing::ParseMessage(bad_magic, &parsed, &error),
        "bad magic is rejected");
  auto truncated = wire;
  truncated.pop_back();
  Check(!routing::ParseMessage(truncated, &parsed, &error),
        "truncated payload is rejected");

  routing::StateSnapshot snapshot;
  snapshot.held_keys = {30, 42};
  snapshot.mouse_buttons = 5;
  snapshot.cursor_x = 100;
  snapshot.cursor_y = 200;
  const auto snapshot_wire = routing::SerializeStateSnapshot(snapshot);
  routing::StateSnapshot parsed_snapshot;
  Check(routing::ParseStateSnapshot(snapshot_wire, &parsed_snapshot, &error),
        "snapshot parses");
  Check(parsed_snapshot.held_keys == snapshot.held_keys &&
            parsed_snapshot.mouse_buttons == 5 &&
            parsed_snapshot.cursor_x == 100,
        "snapshot round trip");
}

void TestQueueCoalescingAndOverload() {
  routing::EventQueue queue({3, std::chrono::milliseconds(10)});
  routing::RoutedEvent move;
  move.kind = routing::EventKind::kMouseMove;
  move.device_key = 1;
  move.value1 = 5;
  move.value2 = 2;
  Check(queue.Push(move), "first motion queues");
  move.value1 = 8;
  move.value2 = -1;
  Check(queue.Push(move), "second motion coalesces");
  const auto motion = queue.Take(10);
  Check(motion.size() == 1 && motion[0].value1 == 13 &&
            motion[0].value2 == 1,
        "relative motion sums");

  routing::RoutedEvent key;
  key.kind = routing::EventKind::kKeyDown;
  key.device_key = 2;
  key.value1 = 30;
  Check(queue.Push(key), "first key queues");
  Check(queue.Push(key), "second key queues discretely");
  Check(queue.Push(key), "third key queues discretely");
  Check(!queue.Push(key), "discrete overload resets route queue");
  Check(queue.size() == 0 && queue.metrics().resets == 1,
        "overload clears queue and increments reset metric");

  const auto base = std::chrono::steady_clock::now();
  Check(queue.Push(key, base), "old event queues");
  Check(!queue.Push(key, base + std::chrono::milliseconds(20)),
        "queue age overload resets");
}

void TestRouteStateSafety() {
  std::vector<routing::RoutedEvent> applied;
  std::vector<routing::StateSnapshot> releases;
  routing::RouteState state(
      [&applied](const routing::RoutedEvent& event) {
        applied.push_back(event);
      },
      [&releases](const routing::StateSnapshot& snapshot) {
        releases.push_back(snapshot);
      });

  routing::SessionId session{};
  session[0] = std::byte{1};
  Check(state.Claim(9, session), "route claim succeeds");
  routing::MessageHeader header;
  header.type = routing::MessageType::kEventBatch;
  header.route_id = 9;
  header.session = session;
  header.sequence = 1;

  routing::RoutedEvent down;
  down.kind = routing::EventKind::kKeyDown;
  down.value1 = 30;
  routing::RoutedEvent button;
  button.kind = routing::EventKind::kMouseButtonDown;
  button.value1 = 0;
  const std::array events{down, button};
  Check(state.ApplyBatch(header, events) == routing::ApplyResult::kApplied,
        "valid batch applies");
  Check(state.state().held_keys.size() == 1 &&
            state.state().mouse_buttons == 1,
        "receiver tracks held state");

  Check(state.ApplyBatch(header, events) == routing::ApplyResult::kDuplicate,
        "duplicate batch rejected");
  header.sequence = 3;
  Check(state.ApplyBatch(header, events) == routing::ApplyResult::kSequenceGap,
        "sequence gap resets route");
  Check(!state.claimed() && releases.size() == 1 &&
            releases[0].held_keys.size() == 1 &&
            releases[0].mouse_buttons == 1,
        "gap releases receiver-held state");

  routing::SessionId other{};
  other[0] = std::byte{2};
  Check(state.Claim(9, session), "route reclaims");
  header.sequence = 1;
  header.session = other;
  Check(state.ApplyBatch(header, events) ==
            routing::ApplyResult::kStaleSession,
        "stale session rejected");
  Check(state.metrics().stale_sessions == 1,
        "stale session is measured");
}

}  // namespace

int main() {
  TestProtocolRoundTrip();
  TestQueueCoalescingAndOverload();
  TestRouteStateSafety();
  if (failures == 0) {
    std::cout << "input routing tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
