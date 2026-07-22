#include "src/common/configuration.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace streaming {
namespace {

bool Fail(std::string message, std::string* error) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

bool RequireMap(const YAML::Node& node,
                const char* context,
                std::string* error) {
  if (!node.IsMap()) {
    return Fail(std::string(context) + " must be a mapping", error);
  }
  return true;
}

bool RejectUnknownKeys(const YAML::Node& node,
                       std::initializer_list<std::string_view> allowed,
                       const char* context,
                       std::string* error) {
  std::unordered_set<std::string_view> allowed_set(allowed);
  for (const auto& item : node) {
    if (!item.first.IsScalar()) {
      return Fail(std::string(context) + " contains a non-scalar key", error);
    }
    const std::string key = item.first.as<std::string>();
    if (!allowed_set.contains(key)) {
      return Fail(std::string(context) + " contains unknown key '" + key + "'",
                  error);
    }
  }
  return true;
}

template <typename T>
bool ReadScalar(const YAML::Node& parent,
                const char* key,
                const char* context,
                T* value,
                std::string* error) {
  const YAML::Node node = parent[key];
  if (!node) {
    return true;
  }
  if (!node.IsScalar()) {
    return Fail(std::string(context) + "." + key + " must be a scalar", error);
  }
  try {
    *value = node.as<T>();
  } catch (const YAML::Exception& exception) {
    return Fail(std::string(context) + "." + key + ": " + exception.what(),
                error);
  }
  return true;
}

bool ValidateRange(int value,
                   int minimum,
                   int maximum,
                   const char* name,
                   std::string* error) {
  if (value < minimum || value > maximum) {
    return Fail(std::string(name) + " must be between " +
                    std::to_string(minimum) + " and " +
                    std::to_string(maximum),
                error);
  }
  return true;
}

bool ParseProducer(const YAML::Node& node,
                   ProducerConfiguration* configuration,
                   std::string* error) {
  if (!RequireMap(node, "producer configuration", error) ||
      !RejectUnknownKeys(node,
                         {"url", "force_transparency", "viewer_visible",
                          "alpha_probe_enabled", "viewport", "frame_rate"},
                         "producer", error)) {
    return false;
  }

  if (!ReadScalar(node, "url", "producer", &configuration->url, error) ||
      !ReadScalar(node, "force_transparency", "producer",
                  &configuration->force_transparency, error) ||
      !ReadScalar(node, "viewer_visible", "producer",
                  &configuration->viewer_visible, error) ||
      !ReadScalar(node, "alpha_probe_enabled", "producer",
                  &configuration->alpha_probe_enabled, error) ||
      !ReadScalar(node, "frame_rate", "producer", &configuration->frame_rate,
                  error)) {
    return false;
  }
  if (configuration->url.empty()) {
    return Fail("producer.url must not be empty", error);
  }

  if (const YAML::Node viewport = node["viewport"]) {
    if (!RequireMap(viewport, "producer.viewport", error) ||
        !RejectUnknownKeys(viewport, {"width", "height"},
                           "producer.viewport", error) ||
        !ReadScalar(viewport, "width", "producer.viewport",
                    &configuration->view_width, error) ||
        !ReadScalar(viewport, "height", "producer.viewport",
                    &configuration->view_height, error)) {
      return false;
    }
  }

  return ValidateRange(configuration->view_width, 320, 16384,
                       "producer.viewport.width", error) &&
         ValidateRange(configuration->view_height, 240, 16384,
                       "producer.viewport.height", error) &&
         ValidateRange(configuration->frame_rate, 1, 60,
                       "producer.frame_rate", error);
}

bool ParseViewer(const YAML::Node& node,
                 ViewerConfiguration* configuration,
                 std::string* error) {
  if (!RequireMap(node, "viewer configuration", error) ||
      !RejectUnknownKeys(node, {"navigate", "window"}, "viewer", error) ||
      !ReadScalar(node, "navigate", "viewer", &configuration->navigate,
                  error)) {
    return false;
  }

  if (const YAML::Node window = node["window"]) {
    if (!RequireMap(window, "viewer.window", error) ||
        !RejectUnknownKeys(window,
                           {"width", "height", "show_toolbar",
                            "show_url_bar", "url_bar_overlays_content",
                            "pixel_perfect", "maximized", "fullscreen"},
                           "viewer.window", error) ||
        !ReadScalar(window, "width", "viewer.window",
                    &configuration->window_width, error) ||
        !ReadScalar(window, "height", "viewer.window",
                    &configuration->window_height, error) ||
        !ReadScalar(window, "show_toolbar", "viewer.window",
                    &configuration->show_toolbar, error) ||
        !ReadScalar(window, "show_url_bar", "viewer.window",
                    &configuration->show_url_bar, error) ||
        !ReadScalar(window, "url_bar_overlays_content", "viewer.window",
                    &configuration->url_bar_overlays_content, error) ||
        !ReadScalar(window, "pixel_perfect", "viewer.window",
                    &configuration->pixel_perfect, error) ||
        !ReadScalar(window, "maximized", "viewer.window",
                    &configuration->maximized, error) ||
        !ReadScalar(window, "fullscreen", "viewer.window",
                    &configuration->fullscreen, error)) {
      return false;
    }
  }

  if (configuration->maximized && configuration->fullscreen) {
    return Fail("viewer.window.maximized and fullscreen cannot both be true",
                error);
  }

  return ValidateRange(configuration->window_width, 320, 16384,
                       "viewer.window.width", error) &&
         ValidateRange(configuration->window_height, 240, 16384,
                       "viewer.window.height", error);
}

template <typename Configuration>
bool ParseWithRoot(std::string_view yaml,
                   bool (*parse_root)(const YAML::Node&, Configuration*,
                                      std::string*),
                   Configuration* configuration,
                   std::string* error) {
  if (configuration == nullptr) {
    return Fail("configuration output is null", error);
  }
  try {
    const YAML::Node root = YAML::Load(std::string(yaml));
    Configuration parsed;
    if (!parse_root(root, &parsed, error)) {
      return false;
    }
    *configuration = std::move(parsed);
    return true;
  } catch (const YAML::Exception& exception) {
    return Fail(std::string("invalid YAML: ") + exception.what(), error);
  }
}

template <typename Configuration>
bool LoadWithParser(const std::filesystem::path& path,
                    bool (*parse_yaml)(std::string_view, Configuration*,
                                       std::string*),
                    Configuration* configuration,
                    std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Fail("could not open configuration file: " + path.string(), error);
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  if (!stream.good() && !stream.eof()) {
    return Fail("could not read configuration file: " + path.string(), error);
  }
  if (!parse_yaml(contents.str(), configuration, error)) {
    if (error != nullptr) {
      *error = path.string() + ": " + *error;
    }
    return false;
  }
  return true;
}

}  // namespace

bool ParseProducerConfigurationYaml(std::string_view yaml,
                                    ProducerConfiguration* configuration,
                                    std::string* error) {
  return ParseWithRoot(yaml, &ParseProducer, configuration, error);
}

bool LoadProducerConfigurationYaml(const std::filesystem::path& path,
                                   ProducerConfiguration* configuration,
                                   std::string* error) {
  return LoadWithParser(path, &ParseProducerConfigurationYaml, configuration,
                        error);
}

bool ParseViewerConfigurationYaml(std::string_view yaml,
                                  ViewerConfiguration* configuration,
                                  std::string* error) {
  return ParseWithRoot(yaml, &ParseViewer, configuration, error);
}

bool LoadViewerConfigurationYaml(const std::filesystem::path& path,
                                 ViewerConfiguration* configuration,
                                 std::string* error) {
  return LoadWithParser(path, &ParseViewerConfigurationYaml, configuration,
                        error);
}

}  // namespace streaming
