set(VIEWER_TARGET streaming_viewer)
set(VIEWER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")
set(CEF_TARGET_OUT_DIR "${VIEWER_OUTPUT_DIR}")

add_executable(${VIEWER_TARGET} WIN32
  d3d_renderer.cc
  d3d_renderer.h
  stream_client.cc
  stream_client.h
  viewer_main.cc
)

SET_EXECUTABLE_TARGET_PROPERTIES(${VIEWER_TARGET})
set_target_properties(${VIEWER_TARGET} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${VIEWER_OUTPUT_DIR}")
target_compile_features(${VIEWER_TARGET} PRIVATE cxx_std_20)
target_include_directories(${VIEWER_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(${VIEWER_TARGET} PRIVATE
  streaming_common
  d3d11.lib
  dxgi.lib
  d3dcompiler.lib)
target_link_libraries(${VIEWER_TARGET} PRIVATE shell32.lib)
target_link_libraries(${VIEWER_TARGET} PRIVATE imm32.lib)
ADD_WINDOWS_MANIFEST("${CMAKE_CURRENT_SOURCE_DIR}/win" "${VIEWER_TARGET}" "exe")
