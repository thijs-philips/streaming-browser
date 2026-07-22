#include "src/common/configuration.h"

#include <iostream>
#include <string>

namespace {

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  streaming::ApplicationConfiguration configuration;
  std::string error;
  constexpr std::string_view yaml = R"YAML(
producer:
  url: https://example.org/dashboard
  force_transparency: true
  viewer_visible: true
  alpha_probe_enabled: false
  viewport:
    width: 1920
    height: 1080
  frame_rate: 24
viewer:
  navigate: https://example.org/controls
  window:
    width: 1440
    height: 900
    toolbar_visible: false
    pixel_perfect: true
    fullscreen: true
)YAML";

  if (!streaming::ParseConfigurationYaml(yaml, &configuration, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (configuration.producer.url != "https://example.org/dashboard" ||
      !configuration.producer.force_transparency ||
      !configuration.producer.viewer_visible ||
      configuration.producer.view_width != 1920 ||
      configuration.producer.view_height != 1080 ||
      configuration.producer.frame_rate != 24) {
    return Fail("producer YAML values did not parse");
  }
  if (configuration.viewer.navigate != "https://example.org/controls" ||
      configuration.viewer.window_width != 1440 ||
      configuration.viewer.window_height != 900 ||
      configuration.viewer.toolbar_visible ||
      !configuration.viewer.pixel_perfect ||
      !configuration.viewer.fullscreen) {
    return Fail("viewer YAML values did not parse");
  }

  if (streaming::ParseConfigurationYaml(
          "producer:\n  frame_rae: 30\n", &configuration, &error) ||
      error.find("unknown key 'frame_rae'") == std::string::npos) {
    return Fail("unknown YAML key was not rejected");
  }
  if (streaming::ParseConfigurationYaml(
          "producer:\n  frame_rate: 0\n", &configuration, &error) ||
      error.find("between 1 and 60") == std::string::npos) {
    return Fail("out-of-range frame rate was not rejected");
  }
  if (streaming::ParseConfigurationYaml(
          "viewer:\n  window:\n    width: wide\n", &configuration,
          &error) ||
      error.find("viewer.window.width") == std::string::npos) {
    return Fail("invalid scalar type was not rejected");
  }

  if (!streaming::ParseConfigurationYaml("{}", &configuration, &error) ||
      configuration.producer.view_width != 3840 ||
      configuration.viewer.window_width != 1280) {
    return Fail("empty YAML did not preserve defaults");
  }

  std::cout << "configuration tests passed\n";
  return 0;
}
