#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace streaming::compositor {

inline constexpr std::size_t kMaximumViewports = 8;
inline constexpr std::size_t kMaximumLayoutMessageSize = 64 * 1024;
inline constexpr std::uint32_t kLayoutProtocolVersion = 1;

struct NormalizedRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

struct LayoutViewport {
  std::string viewport_id;
  std::string source_id;
  std::string label;
  NormalizedRect rect;
};

struct LayoutSnapshot {
  std::uint64_t revision = 0;
  std::vector<LayoutViewport> viewports;
};

bool ParseLayoutMessage(std::string_view json,
                        LayoutSnapshot* snapshot,
                        std::string* error);

std::string SerializeHello();
std::string SerializeApplied(std::uint64_t revision);
std::string SerializeLayoutError(std::string_view code);

}  // namespace streaming::compositor
