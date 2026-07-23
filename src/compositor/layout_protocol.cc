#include "src/compositor/layout_protocol.h"

#include <boost/json.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace streaming::compositor {
namespace {

namespace json = boost::json;

bool Fail(std::string message, std::string* error) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

bool HasExactKeys(const json::object& object,
                  std::initializer_list<std::string_view> keys,
                  std::string_view context,
                  std::string* error) {
  const std::unordered_set<std::string_view> expected(keys);
  if (object.size() != expected.size()) {
    return Fail(std::string(context) + " has missing or unknown fields", error);
  }
  for (const auto& item : object) {
    if (!expected.contains(item.key())) {
      return Fail(std::string(context) + " contains unknown field '" +
                      std::string(item.key()) + "'",
                  error);
    }
  }
  return true;
}

bool ReadString(const json::object& object,
                std::string_view key,
                std::size_t maximum_length,
                std::string* output,
                std::string* error) {
  const auto* value = object.if_contains(key);
  if (value == nullptr || !value->is_string()) {
    return Fail(std::string(key) + " must be a string", error);
  }
  const auto& text = value->as_string();
  if (text.empty() || text.size() > maximum_length) {
    return Fail(std::string(key) + " has invalid length", error);
  }
  *output = std::string(text);
  return true;
}

bool ReadNumber(const json::object& object,
                std::string_view key,
                double* output,
                std::string* error) {
  const auto* value = object.if_contains(key);
  if (value == nullptr || (!value->is_double() && !value->is_int64() &&
                           !value->is_uint64())) {
    return Fail(std::string(key) + " must be a number", error);
  }
  const double number = value->to_number<double>();
  if (!std::isfinite(number)) {
    return Fail(std::string(key) + " must be finite", error);
  }
  *output = number;
  return true;
}

bool ParseViewport(const json::value& value,
                   LayoutViewport* viewport,
                   std::string* error) {
  if (!value.is_object()) return Fail("viewport must be an object", error);
  const auto& object = value.as_object();
  if (!HasExactKeys(object,
                    {"viewportId", "sourceId", "label", "x", "y", "width",
                     "height"},
                    "viewport", error) ||
      !ReadString(object, "viewportId", 128, &viewport->viewport_id, error) ||
      !ReadString(object, "sourceId", 128, &viewport->source_id, error) ||
      !ReadString(object, "label", 256, &viewport->label, error) ||
      !ReadNumber(object, "x", &viewport->rect.x, error) ||
      !ReadNumber(object, "y", &viewport->rect.y, error) ||
      !ReadNumber(object, "width", &viewport->rect.width, error) ||
      !ReadNumber(object, "height", &viewport->rect.height, error)) {
    return false;
  }

  constexpr double kTolerance = 0.000001;
  if (viewport->rect.x < 0.0 || viewport->rect.y < 0.0 ||
      viewport->rect.width <= 0.0 || viewport->rect.height <= 0.0 ||
      viewport->rect.x + viewport->rect.width > 1.0 + kTolerance ||
      viewport->rect.y + viewport->rect.height > 1.0 + kTolerance) {
    return Fail("viewport rectangle is outside normalized output bounds", error);
  }
  return true;
}

}  // namespace

bool ParseLayoutMessage(std::string_view text,
                        LayoutSnapshot* snapshot,
                        std::string* error) {
  if (snapshot == nullptr) return Fail("layout output is null", error);
  if (text.empty() || text.size() > kMaximumLayoutMessageSize) {
    return Fail("layout message size is invalid", error);
  }

  boost::system::error_code parse_error;
  const json::value root = json::parse(text, parse_error);
  if (parse_error || !root.is_object()) {
    return Fail("layout message is not valid JSON object", error);
  }
  const auto& object = root.as_object();
  if (!HasExactKeys(object, {"type", "protocol", "revision", "viewports"},
                    "layout", error)) {
    return false;
  }
  const auto* type = object.if_contains("type");
  const auto* protocol = object.if_contains("protocol");
  const auto* revision = object.if_contains("revision");
  const auto* viewports = object.if_contains("viewports");
  if (type == nullptr || !type->is_string() || type->as_string() != "layout") {
    return Fail("type must be 'layout'", error);
  }
  if (protocol == nullptr || !protocol->is_int64() ||
      protocol->as_int64() != kLayoutProtocolVersion) {
    return Fail("unsupported layout protocol", error);
  }
  if (revision == nullptr ||
      (!revision->is_uint64() && !revision->is_int64())) {
    return Fail("revision must be a positive integer", error);
  }
  std::uint64_t revision_number = 0;
  if (revision->is_uint64()) {
    revision_number = revision->as_uint64();
  } else if (revision->as_int64() > 0) {
    revision_number = static_cast<std::uint64_t>(revision->as_int64());
  }
  if (revision_number == 0) return Fail("revision must be positive", error);
  if (viewports == nullptr || !viewports->is_array()) {
    return Fail("viewports must be an array", error);
  }
  const auto& array = viewports->as_array();
  if (array.size() > kMaximumViewports) {
    return Fail("layout contains more than eight viewports", error);
  }

  LayoutSnapshot parsed;
  parsed.revision = revision_number;
  parsed.viewports.reserve(array.size());
  std::unordered_set<std::string> viewport_ids;
  for (const auto& value : array) {
    LayoutViewport viewport;
    if (!ParseViewport(value, &viewport, error)) return false;
    if (!viewport_ids.insert(viewport.viewport_id).second) {
      return Fail("layout contains duplicate viewportId", error);
    }
    parsed.viewports.push_back(std::move(viewport));
  }
  *snapshot = std::move(parsed);
  return true;
}

std::string SerializeHello() {
  json::object value;
  value["type"] = "hello";
  value["protocol"] = kLayoutProtocolVersion;
  value["maxViewports"] = kMaximumViewports;
  return json::serialize(value);
}

std::string SerializeApplied(std::uint64_t revision) {
  json::object value;
  value["type"] = "applied";
  value["protocol"] = kLayoutProtocolVersion;
  value["revision"] = revision;
  return json::serialize(value);
}

std::string SerializeLayoutError(std::string_view code) {
  json::object value;
  value["type"] = "error";
  value["protocol"] = kLayoutProtocolVersion;
  value["code"] = code;
  return json::serialize(value);
}

}  // namespace streaming::compositor
