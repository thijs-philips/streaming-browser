add_executable(configuration_tests
  configuration_tests.cc
)
target_link_libraries(configuration_tests PRIVATE streaming_common)
target_include_directories(configuration_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_features(configuration_tests PRIVATE cxx_std_20)
target_compile_definitions(configuration_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(configuration_tests PRIVATE /W4 /WX /permissive- /utf-8)
set_target_properties(configuration_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME configuration_tests COMMAND configuration_tests)
set_tests_properties(configuration_tests PROPERTIES LABELS "unit;configuration")

add_executable(protocol_tests
  protocol_tests.cc
)
target_link_libraries(protocol_tests PRIVATE streaming_common)
target_include_directories(protocol_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_features(protocol_tests PRIVATE cxx_std_20)
target_compile_definitions(protocol_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(protocol_tests PRIVATE /W4 /WX /permissive- /utf-8)
set_target_properties(protocol_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME protocol_tests COMMAND protocol_tests)
set_tests_properties(protocol_tests PROPERTIES LABELS "unit")

add_executable(pipe_tests pipe_tests.cc)
target_link_libraries(pipe_tests PRIVATE streaming_common)
target_include_directories(pipe_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_features(pipe_tests PRIVATE cxx_std_20)
target_compile_definitions(pipe_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pipe_tests PRIVATE /W4 /WX /permissive- /utf-8)
set_target_properties(pipe_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME pipe_tests COMMAND pipe_tests)
set_tests_properties(pipe_tests PROPERTIES LABELS "unit;ipc")

add_executable(input_routing_tests input_routing_tests.cc)
target_link_libraries(input_routing_tests PRIVATE streaming_input_routing)
target_include_directories(input_routing_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_features(input_routing_tests PRIVATE cxx_std_20)
target_compile_definitions(input_routing_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(input_routing_tests PRIVATE
  /W4 /WX /permissive- /utf-8)
set_target_properties(input_routing_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME input_routing_tests COMMAND input_routing_tests)
set_tests_properties(input_routing_tests PROPERTIES LABELS "unit;input-routing")

add_executable(network_input_tests
  network_input_tests.cc
  "${CMAKE_SOURCE_DIR}/src/producer/network_input_server.cc"
  "${CMAKE_SOURCE_DIR}/src/producer/network_input_server.h"
)
target_link_libraries(network_input_tests PRIVATE
  boost_headers streaming_common streaming_input_routing ws2_32.lib)
target_include_directories(network_input_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_features(network_input_tests PRIVATE cxx_std_20)
target_compile_definitions(network_input_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(network_input_tests PRIVATE
  /W4 /WX /permissive- /utf-8 /wd4267)
set_target_properties(network_input_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME network_input_tests COMMAND network_input_tests)
set_tests_properties(network_input_tests PROPERTIES LABELS "integration;input-routing")

add_executable(compositor_configuration_tests
  compositor_configuration_tests.cc)
target_link_libraries(compositor_configuration_tests PRIVATE compositor_core)
target_include_directories(compositor_configuration_tests PRIVATE
  "${CMAKE_SOURCE_DIR}")
target_compile_features(compositor_configuration_tests PRIVATE cxx_std_20)
target_compile_definitions(compositor_configuration_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(compositor_configuration_tests PRIVATE
  /W4 /WX /permissive- /utf-8)
set_target_properties(compositor_configuration_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME compositor_configuration_tests COMMAND compositor_configuration_tests)
set_tests_properties(compositor_configuration_tests PROPERTIES
  LABELS "unit;configuration;compositor")

add_executable(compositor_layout_protocol_tests
  compositor_layout_protocol_tests.cc)
target_link_libraries(compositor_layout_protocol_tests PRIVATE compositor_core)
target_include_directories(compositor_layout_protocol_tests PRIVATE
  "${CMAKE_SOURCE_DIR}")
target_compile_features(compositor_layout_protocol_tests PRIVATE cxx_std_20)
target_compile_definitions(compositor_layout_protocol_tests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(compositor_layout_protocol_tests PRIVATE
  /W4 /WX /permissive- /utf-8)
set_target_properties(compositor_layout_protocol_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
add_test(NAME compositor_layout_protocol_tests COMMAND compositor_layout_protocol_tests)
set_tests_properties(compositor_layout_protocol_tests PROPERTIES
  LABELS "unit;protocol;compositor")
