#include "src/compositor/layout_protocol.h"

#include <iostream>
#include <string>

namespace {
int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}
}  // namespace

int main() {
  streaming::compositor::LayoutSnapshot snapshot;
  std::string error;
  constexpr std::string_view valid = R"JSON({
    "type":"layout","protocol":1,"revision":7,"viewports":[
      {"viewportId":"left","sourceId":"live-xray","label":"Live X-ray",
       "x":0.1,"y":0.2,"width":0.3,"height":0.4}
    ]})JSON";
  if (!streaming::compositor::ParseLayoutMessage(valid, &snapshot, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  if (snapshot.revision != 7 || snapshot.viewports.size() != 1 ||
      snapshot.viewports[0].source_id != "live-xray") {
    return Fail("valid layout did not parse");
  }
  if (streaming::compositor::ParseLayoutMessage(
          R"JSON({"type":"layout","protocol":1,"revision":1,"viewports":[{"viewportId":"a","sourceId":"s","label":"l","x":0.8,"y":0,"width":0.3,"height":1}]})JSON",
          &snapshot, &error) ||
      error.find("outside") == std::string::npos) {
    return Fail("out-of-bounds rectangle was not rejected");
  }
  if (streaming::compositor::ParseLayoutMessage(
          R"JSON({"type":"layout","protocol":1,"revision":1,"viewports":[{"viewportId":"a","sourceId":"s","label":"l","x":0,"y":0,"width":0.5,"height":1},{"viewportId":"a","sourceId":"t","label":"m","x":0.5,"y":0,"width":0.5,"height":1}]})JSON",
          &snapshot, &error) ||
      error.find("duplicate") == std::string::npos) {
    return Fail("duplicate viewport id was not rejected");
  }
  if (streaming::compositor::ParseLayoutMessage(
          R"JSON({"type":"layout","protocol":1,"revision":1,"viewports":[],"extra":true})JSON",
          &snapshot, &error) ||
      error.find("unknown") == std::string::npos) {
    return Fail("unknown layout field was not rejected");
  }
  if (streaming::compositor::SerializeHello().find("maxViewports") ==
      std::string::npos) {
    return Fail("hello response is incomplete");
  }
  std::cout << "compositor layout protocol tests passed\n";
  return 0;
}
