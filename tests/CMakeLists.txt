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
