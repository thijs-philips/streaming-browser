#include "src/compositor/compositor_configuration.h"

#include <yaml-cpp/yaml.h>

#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace streaming::compositor {
namespace {

bool Fail(std::string message, std::string* error) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

bool RequireMap(const YAML::Node& node,
                std::string_view context,
                std::string* error) {
  return node.IsMap()
             ? true
             : Fail(std::string(context) + " must be a mapping", error);
}

bool RejectUnknownKeys(const YAML::Node& node,
                       std::initializer_list<std::string_view> allowed,
                       std::string_view context,
                       std::string* error) {
  const std::unordered_set<std::string_view> allowed_set(allowed);
  for (const auto& item : node) {
    if (!item.first.IsScalar()) {
      return Fail(std::string(context) + " contains a non-scalar key", error);
    }
    const std::string key = item.first.as<std::string>();
    if (!allowed_set.contains(key)) {
      return Fail(std::string(context) + " contains unknown key '" + key +
                      "'",
                  error);
    }
  }
  return true;
}

template <typename T>
bool ReadScalar(const YAML::Node& parent,
                const char* key,
                std::string_view context,
                T* value,
                std::string* error) {
  const YAML::Node node = parent[key];
  if (!node) return true;
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

bool InRange(int value,
             int minimum,
             int maximum,
             std::string_view name,
             std::string* error) {
  if (value < minimum || value > maximum) {
    return Fail(std::string(name) + " must be between " +
                    std::to_string(minimum) + " and " +
                    std::to_string(maximum),
                error);
  }
  return true;
}

bool ParseRoot(const YAML::Node& root,
               CompositorConfiguration* configuration,
               std::string* error) {
  if (!RequireMap(root, "compositor configuration", error) ||
      !RejectUnknownKeys(root, {"websocket", "output", "window"},
                         "compositor configuration", error)) {
    return false;
  }

  if (const YAML::Node websocket = root["websocket"]) {
    int port = configuration->websocket_port;
    if (!RequireMap(websocket, "websocket", error) ||
        !RejectUnknownKeys(websocket, {"bind_address", "port", "path"},
                           "websocket", error) ||
        !ReadScalar(websocket, "bind_address", "websocket",
                    &configuration->bind_address, error) ||
        !ReadScalar(websocket, "port", "websocket", &port, error) ||
        !ReadScalar(websocket, "path", "websocket",
                    &configuration->websocket_path, error) ||
        !InRange(port, 1024, 65535, "websocket.port", error)) {
      return false;
    }
    configuration->websocket_port = static_cast<std::uint16_t>(port);
  }

  if (configuration->bind_address != "127.0.0.1" &&
      configuration->bind_address != "::1") {
    return Fail("websocket.bind_address must be loopback", error);
  }
  if (configuration->websocket_path.empty() ||
      configuration->websocket_path.front() != '/') {
    return Fail("websocket.path must start with '/'", error);
  }

  if (const YAML::Node output = root["output"]) {
    int width = static_cast<int>(configuration->output_width);
    int height = static_cast<int>(configuration->output_height);
    if (!RequireMap(output, "output", error) ||
        !RejectUnknownKeys(output, {"width", "height"}, "output", error) ||
        !ReadScalar(output, "width", "output", &width, error) ||
        !ReadScalar(output, "height", "output", &height, error) ||
        !InRange(width, 320, 16384, "output.width", error) ||
        !InRange(height, 240, 16384, "output.height", error)) {
      return false;
    }
    configuration->output_width = static_cast<std::uint32_t>(width);
    configuration->output_height = static_cast<std::uint32_t>(height);
  }

  if (const YAML::Node window = root["window"]) {
    std::string scaling = configuration->server_scaling ? "server" : "client";
    if (!RequireMap(window, "window", error) ||
        !RejectUnknownKeys(window,
                           {"width", "height", "maximized", "fullscreen",
                            "monitor", "scaling"},
                           "window", error) ||
        !ReadScalar(window, "scaling", "window", &scaling, error) ||
        !ReadScalar(window, "width", "window", &configuration->window_width,
                    error) ||
        !ReadScalar(window, "height", "window", &configuration->window_height,
                    error) ||
        !ReadScalar(window, "maximized", "window", &configuration->maximized,
                    error) ||
        !ReadScalar(window, "fullscreen", "window", &configuration->fullscreen,
                    error) ||
        !ReadScalar(window, "monitor", "window", &configuration->monitor,
                    error) ||
        !InRange(configuration->window_width, 320, 16384, "window.width",
                 error) ||
        !InRange(configuration->window_height, 240, 16384, "window.height",
                 error) ||
        !InRange(configuration->monitor, 0, 63, "window.monitor", error)) {
      return false;
    }
    if (scaling != "server" && scaling != "client") {
      return Fail("window.scaling must be 'server' or 'client'", error);
    }
    configuration->server_scaling = scaling == "server";
  }

  if (configuration->maximized && configuration->fullscreen) {
    return Fail("window.maximized and window.fullscreen cannot both be true",
                error);
  }
  return true;
}

}  // namespace

bool ParseCompositorConfigurationYaml(std::string_view yaml,
                                      CompositorConfiguration* configuration,
                                      std::string* error) {
  if (configuration == nullptr) return Fail("configuration output is null", error);
  try {
    const YAML::Node root = YAML::Load(std::string(yaml));
    CompositorConfiguration parsed;
    if (!ParseRoot(root, &parsed, error)) return false;
    *configuration = std::move(parsed);
    return true;
  } catch (const YAML::Exception& exception) {
    return Fail(std::string("invalid YAML: ") + exception.what(), error);
  }
}

bool LoadCompositorConfigurationYaml(const std::filesystem::path& path,
                                     CompositorConfiguration* configuration,
                                     std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Fail("could not open compositor configuration: " + path.string(),
                error);
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  if (!stream.good() && !stream.eof()) {
    return Fail("could not read compositor configuration: " + path.string(),
                error);
  }
  if (!ParseCompositorConfigurationYaml(contents.str(), configuration, error)) {
    if (error != nullptr) *error = path.string() + ": " + *error;
    return false;
  }
  return true;
}

}  // namespace streaming::compositor
