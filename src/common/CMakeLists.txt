add_library(streaming_common STATIC
  configuration.cc
  configuration.h
  logging.cc
  pipe_io.cc
  pipe_security.cc
  protocol.cc
)

target_include_directories(streaming_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_features(streaming_common PUBLIC cxx_std_20)
target_compile_definitions(streaming_common PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(streaming_common PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(streaming_common PUBLIC advapi32.lib yaml-cpp::yaml-cpp)
