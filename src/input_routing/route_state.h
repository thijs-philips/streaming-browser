#pragma once

#include "src/input_routing/routing_protocol.h"

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

namespace streaming::input_routing {

enum class ApplyResult {
  kApplied,
  kDuplicate,
  kSequenceGap,
  kStaleSession,
  kNoRoute,
  kInvalidState,
};

struct RouteStateMetrics {
  std::uint64_t applied_events = 0;
  std::uint64_t duplicate_events = 0;
  std::uint64_t sequence_gaps = 0;
  std::uint64_t stale_sessions = 0;
  std::uint64_t resets = 0;
};

class RouteState final {
 public:
  using EventCallback = std::function<void(const RoutedEvent&)>;
  using ReleaseCallback = std::function<void(const StateSnapshot&)>;

  RouteState(EventCallback event_callback, ReleaseCallback release_callback);

  bool Claim(RouteId route_id, const SessionId& session);
  ApplyResult ApplyBatch(const MessageHeader& header,
                         std::span<const RoutedEvent> events);
  void ApplySnapshot(const MessageHeader& header,
                     const StateSnapshot& snapshot);
  void ReleaseAll();

  [[nodiscard]] bool claimed() const { return claimed_; }
  [[nodiscard]] RouteId route_id() const { return route_id_; }
  [[nodiscard]] std::uint64_t next_sequence() const { return next_sequence_; }
  [[nodiscard]] const StateSnapshot& state() const { return state_; }
  [[nodiscard]] const RouteStateMetrics& metrics() const { return metrics_; }

 private:
  bool Matches(const MessageHeader& header) const;
  bool ApplyEvent(const RoutedEvent& event);
  void ClearState();

  EventCallback event_callback_;
  ReleaseCallback release_callback_;
  bool claimed_ = false;
  RouteId route_id_ = 0;
  SessionId session_{};
  std::uint64_t next_sequence_ = 1;
  StateSnapshot state_;
  std::unordered_set<std::uint32_t> held_keys_;
  RouteStateMetrics metrics_;
};

}  // namespace streaming::input_routing
