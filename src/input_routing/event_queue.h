#pragma once

#include "src/input_routing/routing_protocol.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace streaming::input_routing {

struct EventQueueLimits {
  std::size_t max_events = 1024;
  std::chrono::milliseconds max_age{50};
};

struct EventQueueMetrics {
  std::uint64_t pushed = 0;
  std::uint64_t coalesced = 0;
  std::uint64_t resets = 0;
  std::size_t maximum_size = 0;
};

class EventQueue final {
 public:
  explicit EventQueue(EventQueueLimits limits = {});

  // Returns false after overload. The caller must reset the route rather than
  // replaying stale discrete transitions.
  bool Push(RoutedEvent event,
            std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now());

  std::vector<RoutedEvent> Take(std::size_t maximum);
  void Clear();

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] EventQueueMetrics metrics() const;

 private:
  struct QueuedEvent {
    RoutedEvent event;
    std::chrono::steady_clock::time_point enqueued_at;
  };

  static bool CanCoalesce(const RoutedEvent& left, const RoutedEvent& right);
  static void Coalesce(RoutedEvent* destination, const RoutedEvent& source);

  EventQueueLimits limits_;
  mutable std::mutex mutex_;
  std::deque<QueuedEvent> events_;
  EventQueueMetrics metrics_;
};

}  // namespace streaming::input_routing
