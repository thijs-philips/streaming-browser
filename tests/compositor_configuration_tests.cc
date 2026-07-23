#include "src/compositor/compositor_configuration.h"

#include <iostream>
#include <string>

namespace {
int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}
}  // namespace

int main() {
  streaming::compositor::CompositorConfiguration configuration;
  std::string error;
  constexpr std::string_view valid = R"YAML(
websocket:
  bind_address: 127.0.0.1
  port: 9876
  path: /layout/v1
output:
  width: 3840
  height: 2160
window:
  width: 1280
  height: 720
  maximized: false
  fullscreen: false
  monitor: 1
)YAML";
  if (!streaming::compositor::ParseCompositorConfigurationYaml(
          valid, &configuration, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (configuration.websocket_port != 9876 ||
      configuration.output_width != 3840 || configuration.window_width != 1280 ||
      configuration.monitor != 1) {
    return Fail("compositor configuration values did not parse");
  }
  if (!configuration.server_scaling) {
    return Fail("scaling should default to server");
  }
  if (!streaming::compositor::ParseCompositorConfigurationYaml(
          "window:\n  scaling: client\n", &configuration, &error) ||
      configuration.server_scaling) {
    return Fail("window.scaling client was not applied");
  }
  if (streaming::compositor::ParseCompositorConfigurationYaml(
          "window:\n  scaling: sideways\n", &configuration, &error) ||
      error.find("must be 'server' or 'client'") == std::string::npos) {
    return Fail("invalid window.scaling value was not rejected");
  }
  if (streaming::compositor::ParseCompositorConfigurationYaml(
          "websocket:\n  bind_address: 0.0.0.0\n", &configuration, &error) ||
      error.find("loopback") == std::string::npos) {
    return Fail("non-loopback bind was not rejected");
  }
  if (streaming::compositor::ParseCompositorConfigurationYaml(
          "window:\n  maximized: true\n  fullscreen: true\n", &configuration,
          &error) ||
      error.find("cannot both") == std::string::npos) {
    return Fail("conflicting window modes were not rejected");
  }
  if (streaming::compositor::ParseCompositorConfigurationYaml(
          "window:\n  widht: 1000\n", &configuration, &error) ||
      error.find("unknown key") == std::string::npos) {
    return Fail("unknown compositor key was not rejected");
  }
  std::cout << "compositor configuration tests passed\n";
  return 0;
}
