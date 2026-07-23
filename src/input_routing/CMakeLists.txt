add_library(streaming_input_routing STATIC
  event_queue.cc
  event_queue.h
  route_state.cc
  route_state.h
  routing_protocol.cc
  routing_protocol.h
)

target_include_directories(streaming_input_routing PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_features(streaming_input_routing PUBLIC cxx_std_20)
target_compile_definitions(streaming_input_routing PUBLIC
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(streaming_input_routing PRIVATE
  /W4 /WX /permissive- /utf-8)
target_link_libraries(streaming_input_routing PUBLIC bcrypt.lib)
