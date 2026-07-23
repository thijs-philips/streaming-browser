cmake_minimum_required(VERSION 3.21)

project(streaming_browser_usb_routing LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "usb-routing experiment currently supports Windows x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(boost_headers
  URL https://github.com/boostorg/boost/releases/download/boost-1.90.0/boost-1.90.0-b2-nodocs.zip
  URL_HASH SHA256=7860F2694B76B51874881D7BFE1A5E9BA40435571C76CA559521E512A178782A
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(boost_headers)
if(NOT boost_headers_POPULATED)
  FetchContent_Populate(boost_headers)
endif()

set(GAMEINPUT_PACKAGE_DIR "${CMAKE_BINARY_DIR}/_deps/gameinput-3.4.259")

# Reuse the production-independent canonical protocol and state machine.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/input_routing"
                 "${CMAKE_BINARY_DIR}/input_routing")

add_library(usb_routing_core STATIC
  src/device_identity.cc
  src/device_identity.h
  src/emergency_chord.cc
  src/emergency_chord.h
)
target_include_directories(usb_routing_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(usb_routing_core PUBLIC
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(usb_routing_core PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(usb_routing_core PUBLIC
  cfgmgr32.lib hid.lib ole32.lib setupapi.lib)

add_executable(usb_routing_probe
  src/main.cc
  src/raw_input_capture.cc
  src/raw_input_capture.h
  src/websocket_client.cc
  src/websocket_client.h
)
target_include_directories(usb_routing_probe PRIVATE "${boost_headers_SOURCE_DIR}")
target_link_libraries(usb_routing_probe PRIVATE
  streaming_input_routing usb_routing_core user32.lib ws2_32.lib)
target_compile_definitions(usb_routing_probe PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(usb_routing_probe PRIVATE /W4 /WX /permissive- /utf-8)
set_source_files_properties(src/websocket_client.cc PROPERTIES
  COMPILE_OPTIONS "/wd4267")
set_target_properties(usb_routing_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")

if(EXISTS "${GAMEINPUT_PACKAGE_DIR}/native/include/GameInput.h")
  add_executable(gameinput_probe src/gameinput_v0_probe.cc)
  target_include_directories(gameinput_probe PRIVATE
    "${GAMEINPUT_PACKAGE_DIR}/native/include/v0"
    "${GAMEINPUT_PACKAGE_DIR}/native/include")
  target_compile_definitions(gameinput_probe PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_compile_options(gameinput_probe PRIVATE /W4 /WX /permissive- /utf-8)
  target_link_libraries(gameinput_probe PRIVATE
    "${GAMEINPUT_PACKAGE_DIR}/native/lib/x64/GameInput.lib")
  set_target_properties(gameinput_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
endif()

enable_testing()
add_executable(emergency_chord_tests tests/emergency_chord_tests.cc)
target_link_libraries(emergency_chord_tests PRIVATE usb_routing_core)
target_compile_definitions(emergency_chord_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(emergency_chord_tests PRIVATE
  /W4 /WX /permissive- /utf-8)
set_target_properties(emergency_chord_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME emergency_chord_tests COMMAND emergency_chord_tests)
