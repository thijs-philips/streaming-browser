#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace streaming {

struct ProducerConfiguration {
  std::string url = "https://example.com";
  bool force_transparency = false;
  bool viewer_visible = false;
  bool alpha_probe_enabled = false;
  int view_width = 3840;
  int view_height = 2160;
  int frame_rate = 30;
};

struct ViewerConfiguration {
  std::string navigate;
  int window_width = 1280;
  int window_height = 760;
  bool toolbar_visible = true;
  bool pixel_perfect = false;
  bool fullscreen = false;
};

struct ApplicationConfiguration {
  ProducerConfiguration producer;
  ViewerConfiguration viewer;
};

// Parse a strict, bounded YAML configuration. Unknown keys are rejected so
// misspelled settings cannot silently fall back to defaults.
bool ParseConfigurationYaml(std::string_view yaml,
                            ApplicationConfiguration* configuration,
                            std::string* error);

bool LoadConfigurationYaml(const std::filesystem::path& path,
                           ApplicationConfiguration* configuration,
                           std::string* error);

}  // namespace streaming
