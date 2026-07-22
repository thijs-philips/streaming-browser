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
  std::string error;

  streaming::ProducerConfiguration producer;
  constexpr std::string_view producer_yaml = R"YAML(
url: https://example.org/dashboard
force_transparency: true
viewer_visible: true
alpha_probe_enabled: false
viewport:
  width: 1920
  height: 1080
frame_rate: 24
)YAML";
  if (!streaming::ParseProducerConfigurationYaml(producer_yaml, &producer,
                                                 &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (producer.url != "https://example.org/dashboard" ||
      !producer.force_transparency || !producer.viewer_visible ||
      producer.view_width != 1920 || producer.view_height != 1080 ||
      producer.frame_rate != 24) {
    return Fail("producer YAML values did not parse");
  }

  streaming::ViewerConfiguration viewer;
  constexpr std::string_view viewer_yaml = R"YAML(
navigate: https://example.org/controls
window:
  width: 1440
  height: 900
  show_toolbar: false
  show_url_bar: false
  url_bar_overlays_content: false
  pixel_perfect: true
  maximized: true
  fullscreen: false
)YAML";
  if (!streaming::ParseViewerConfigurationYaml(viewer_yaml, &viewer, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (viewer.navigate != "https://example.org/controls" ||
      viewer.window_width != 1440 || viewer.window_height != 900 ||
      viewer.show_toolbar || viewer.show_url_bar ||
      viewer.url_bar_overlays_content || !viewer.pixel_perfect ||
      !viewer.maximized || viewer.fullscreen) {
    return Fail("viewer YAML values did not parse");
  }

  if (streaming::ParseProducerConfigurationYaml("frame_rae: 30\n", &producer,
                                                &error) ||
      error.find("unknown key 'frame_rae'") == std::string::npos) {
    return Fail("unknown producer YAML key was not rejected");
  }
  if (streaming::ParseProducerConfigurationYaml("frame_rate: 0\n", &producer,
                                                &error) ||
      error.find("between 1 and 60") == std::string::npos) {
    return Fail("out-of-range frame rate was not rejected");
  }
  if (streaming::ParseViewerConfigurationYaml(
          "window:\n  width: wide\n", &viewer, &error) ||
      error.find("viewer.window.width") == std::string::npos) {
    return Fail("invalid scalar type was not rejected");
  }
  if (streaming::ParseViewerConfigurationYaml(
          "window:\n  maximized: true\n  fullscreen: true\n", &viewer,
          &error) ||
      error.find("cannot both be true") == std::string::npos) {
    return Fail("conflicting viewer modes were not rejected");
  }
  if (streaming::ParseViewerConfigurationYaml("navigate: x\nproducer: {}\n",
                                              &viewer, &error) ||
      error.find("unknown key 'producer'") == std::string::npos) {
    return Fail("legacy combined layout was not rejected by viewer parser");
  }

  if (!streaming::ParseProducerConfigurationYaml("{}", &producer, &error) ||
      producer.view_width != 3840 || producer.frame_rate != 30) {
    return Fail("empty producer YAML did not preserve defaults");
  }
  if (streaming::ParseViewerConfigurationYaml(
          "window:\n  toolbar_visible: true\n", &viewer, &error) ||
      error.find("unknown key 'toolbar_visible'") == std::string::npos) {
    return Fail("legacy toolbar_visible key was not rejected");
  }

  if (!streaming::ParseViewerConfigurationYaml("{}", &viewer, &error) ||
      viewer.window_width != 1280 || !viewer.show_toolbar ||
      !viewer.show_url_bar || !viewer.url_bar_overlays_content ||
      viewer.maximized) {
    return Fail("empty viewer YAML did not preserve defaults");
  }

  std::cout << "configuration tests passed\n";
  return 0;
}
