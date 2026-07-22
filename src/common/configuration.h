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
  bool toolbar_overlays_content = true;
  bool pixel_perfect = false;
  bool maximized = false;
  bool fullscreen = false;
};

// Each application owns its own configuration file with its settings at the
// document root. Parsers are strict: unknown keys are rejected so misspelled
// settings cannot silently fall back to defaults.
bool ParseProducerConfigurationYaml(std::string_view yaml,
                                    ProducerConfiguration* configuration,
                                    std::string* error);

bool LoadProducerConfigurationYaml(const std::filesystem::path& path,
                                   ProducerConfiguration* configuration,
                                   std::string* error);

bool ParseViewerConfigurationYaml(std::string_view yaml,
                                  ViewerConfiguration* configuration,
                                  std::string* error);

bool LoadViewerConfigurationYaml(const std::filesystem::path& path,
                                 ViewerConfiguration* configuration,
                                 std::string* error);

}  // namespace streaming
