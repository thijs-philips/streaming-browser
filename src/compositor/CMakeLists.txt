set(COMPOSITOR_TARGET streaming_compositor)
set(COMPOSITOR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
set(CEF_TARGET_OUT_DIR "${COMPOSITOR_OUTPUT_DIR}")

add_library(compositor_core STATIC
  boost_json.cc
  compositor_configuration.cc
  compositor_configuration.h
  layout_protocol.cc
  layout_protocol.h
  layout_server.cc
  layout_server.h
)
target_include_directories(compositor_core PUBLIC "${CMAKE_SOURCE_DIR}")
target_compile_features(compositor_core PUBLIC cxx_std_20)
target_compile_definitions(compositor_core PUBLIC
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
  BOOST_JSON_NO_LIB BOOST_CONTAINER_NO_LIB BOOST_ALL_NO_LIB)
target_compile_options(compositor_core PRIVATE
  /W4 /WX /permissive- /utf-8 /wd4267)
target_link_libraries(compositor_core PUBLIC
  boost_headers streaming_common ws2_32.lib)

add_executable(${COMPOSITOR_TARGET} WIN32
  compositor_main.cc
  compositor_renderer.cc
  compositor_renderer.h
  "${CMAKE_SOURCE_DIR}/src/viewer/stream_client.cc"
  "${CMAKE_SOURCE_DIR}/src/viewer/stream_client.h"
)
set_target_properties(${COMPOSITOR_TARGET} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${COMPOSITOR_OUTPUT_DIR}")
target_compile_features(${COMPOSITOR_TARGET} PRIVATE cxx_std_20)
target_include_directories(${COMPOSITOR_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(${COMPOSITOR_TARGET} PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_compile_options(${COMPOSITOR_TARGET} PRIVATE
  /W4 /WX /permissive- /utf-8)
target_link_libraries(${COMPOSITOR_TARGET} PRIVATE
  compositor_core streaming_common d2d1.lib d3d11.lib d3dcompiler.lib
  dwrite.lib dxgi.lib shell32.lib)
ADD_WINDOWS_MANIFEST("${CMAKE_CURRENT_SOURCE_DIR}/win"
                     "${COMPOSITOR_TARGET}" "exe")
