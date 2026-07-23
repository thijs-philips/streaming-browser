#include "src/input_routing/route_state.h"

#include <algorithm>
#include <utility>

namespace streaming::input_routing {

RouteState::RouteState(EventCallback event_callback,
                       ReleaseCallback release_callback)
    : event_callback_(std::move(event_callback)),
      release_callback_(std::move(release_callback)) {}

bool RouteState::Claim(RouteId route_id, const SessionId& session) {
  if (route_id == 0) return false;
  if (claimed_) ReleaseAll();
  claimed_ = true;
  route_id_ = route_id;
  session_ = session;
  next_sequence_ = 1;
  ClearState();
  return true;
}

ApplyResult RouteState::ApplyBatch(const MessageHeader& header,
                                   std::span<const RoutedEvent> events) {
  if (!claimed_) return ApplyResult::kNoRoute;
  if (!Matches(header)) {
    ++metrics_.stale_sessions;
    return ApplyResult::kStaleSession;
  }
  if (header.sequence < next_sequence_) {
    ++metrics_.duplicate_events;
    return ApplyResult::kDuplicate;
  }
  if (header.sequence != next_sequence_) {
    ++metrics_.sequence_gaps;
    ReleaseAll();
    return ApplyResult::kSequenceGap;
  }
  for (const RoutedEvent& event : events) {
    if (!ApplyEvent(event)) {
      ReleaseAll();
      return ApplyResult::kInvalidState;
    }
    if (event_callback_) event_callback_(event);
    ++metrics_.applied_events;
  }
  ++next_sequence_;
  return ApplyResult::kApplied;
}

void RouteState::ApplySnapshot(const MessageHeader& header,
                               const StateSnapshot& snapshot) {
  if (!Matches(header)) return;
  ClearState();
  state_ = snapshot;
  held_keys_.insert(snapshot.held_keys.begin(), snapshot.held_keys.end());
}

void RouteState::ReleaseAll() {
  if (claimed_ && release_callback_ &&
      (!held_keys_.empty() || state_.mouse_buttons != 0)) {
    state_.held_keys.assign(held_keys_.begin(), held_keys_.end());
    std::ranges::sort(state_.held_keys);
    release_callback_(state_);
  }
  if (claimed_) ++metrics_.resets;
  claimed_ = false;
  route_id_ = 0;
  session_.fill(std::byte{0});
  next_sequence_ = 1;
  ClearState();
}

bool RouteState::Matches(const MessageHeader& header) const {
  return claimed_ && header.route_id == route_id_ &&
         header.session == session_;
}

bool RouteState::ApplyEvent(const RoutedEvent& event) {
  const std::uint32_t key = static_cast<std::uint32_t>(event.value1);
  switch (event.kind) {
    case EventKind::kKeyDown:
      held_keys_.insert(key);
      break;
    case EventKind::kKeyUp:
      held_keys_.erase(key);
      break;
    case EventKind::kMouseButtonDown:
      if (event.value1 < 0 || event.value1 >= 32) return false;
      state_.mouse_buttons |= 1U << static_cast<unsigned int>(event.value1);
      break;
    case EventKind::kMouseButtonUp:
      if (event.value1 < 0 || event.value1 >= 32) return false;
      state_.mouse_buttons &= ~(1U << static_cast<unsigned int>(event.value1));
      break;
    case EventKind::kMouseMove:
      state_.cursor_x += event.value1;
      state_.cursor_y += event.value2;
      break;
    case EventKind::kMouseWheel:
      break;
  }
  state_.held_keys.assign(held_keys_.begin(), held_keys_.end());
  return true;
}

void RouteState::ClearState() {
  state_ = {};
  held_keys_.clear();
}

}  // namespace streaming::input_routing
