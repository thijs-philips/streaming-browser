#include "src/input_routing/event_queue.h"

#include <algorithm>
#include <limits>

namespace streaming::input_routing {
namespace {

std::int32_t SaturatingAdd(std::int32_t left, std::int32_t right) {
  const std::int64_t sum = static_cast<std::int64_t>(left) + right;
  return static_cast<std::int32_t>(std::clamp<std::int64_t>(
      sum, std::numeric_limits<std::int32_t>::min(),
      std::numeric_limits<std::int32_t>::max()));
}

}  // namespace

EventQueue::EventQueue(EventQueueLimits limits) : limits_(limits) {
  limits_.max_events = std::max<std::size_t>(limits_.max_events, 1);
  limits_.max_age = std::max(limits_.max_age, std::chrono::milliseconds(1));
}

bool EventQueue::Push(RoutedEvent event,
                      std::chrono::steady_clock::time_point now) {
  std::lock_guard lock(mutex_);
  ++metrics_.pushed;
  if (!events_.empty() && CanCoalesce(events_.back().event, event)) {
    Coalesce(&events_.back().event, event);
    ++metrics_.coalesced;
    return true;
  }
  if (events_.size() >= limits_.max_events ||
      (!events_.empty() && now - events_.front().enqueued_at > limits_.max_age)) {
    events_.clear();
    ++metrics_.resets;
    return false;
  }
  events_.push_back({std::move(event), now});
  metrics_.maximum_size = std::max(metrics_.maximum_size, events_.size());
  return true;
}

std::vector<RoutedEvent> EventQueue::Take(std::size_t maximum) {
  std::lock_guard lock(mutex_);
  const std::size_t count = std::min(maximum, events_.size());
  std::vector<RoutedEvent> result;
  result.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(std::move(events_.front().event));
    events_.pop_front();
  }
  return result;
}

void EventQueue::Clear() {
  std::lock_guard lock(mutex_);
  events_.clear();
}

std::size_t EventQueue::size() const {
  std::lock_guard lock(mutex_);
  return events_.size();
}

EventQueueMetrics EventQueue::metrics() const {
  std::lock_guard lock(mutex_);
  return metrics_;
}

bool EventQueue::CanCoalesce(const RoutedEvent& left,
                             const RoutedEvent& right) {
  if (left.kind != right.kind || left.device_key != right.device_key ||
      left.flags != right.flags) {
    return false;
  }
  return left.kind == EventKind::kMouseMove ||
         left.kind == EventKind::kMouseWheel;
}

void EventQueue::Coalesce(RoutedEvent* destination,
                          const RoutedEvent& source) {
  destination->timestamp_us = source.timestamp_us;
  destination->value1 = SaturatingAdd(destination->value1, source.value1);
  destination->value2 = SaturatingAdd(destination->value2, source.value2);
  destination->buttons = source.buttons;
  destination->modifiers = source.modifiers;
}

}  // namespace streaming::input_routing
