#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace streaming::compositor {

struct CompositorConfiguration {
  std::string bind_address = "127.0.0.1";
  std::uint16_t websocket_port = 8765;
  std::string websocket_path = "/layout/v1";
  std::uint32_t output_width = 3840;
  std::uint32_t output_height = 2160;
  int window_width = 1440;
  int window_height = 810;
  bool maximized = false;
  bool fullscreen = false;
  int monitor = 0;
  // "server": CEF renders at the compositor's exact client size (default).
  // "client": CEF renders at the fixed logical output size; the compositor
  // scales it. F12 toggles at runtime.
  bool server_scaling = true;
};

bool ParseCompositorConfigurationYaml(std::string_view yaml,
                                      CompositorConfiguration* configuration,
                                      std::string* error);

bool LoadCompositorConfigurationYaml(const std::filesystem::path& path,
                                     CompositorConfiguration* configuration,
                                     std::string* error);

}  // namespace streaming::compositor
