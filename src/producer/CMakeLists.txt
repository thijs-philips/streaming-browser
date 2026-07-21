set(PRODUCER_TARGET streaming_browser)
set(PRODUCER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>")

set(PRODUCER_SOURCES
  app.cc
  app.h
  browser_client.cc
  browser_client.h
  d3d_frame_pipeline.cc
  d3d_frame_pipeline.h
  launcher_window.cc
  launcher_window.h
  producer_entry.cc
)

if(NOT TARGET libcef_lib)
  ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

set(CEF_TARGET_OUT_DIR "${PRODUCER_OUTPUT_DIR}")

if(USE_SANDBOX)
  add_library(${PRODUCER_TARGET} SHARED ${PRODUCER_SOURCES})
  SET_LIBRARY_TARGET_PROPERTIES(${PRODUCER_TARGET})
  set_target_properties(${PRODUCER_TARGET} PROPERTIES PREFIX "")
  COPY_SINGLE_FILE(${PRODUCER_TARGET}
    "${CEF_BINARY_DIR}/bootstrap.exe"
    "${PRODUCER_OUTPUT_DIR}/${PRODUCER_TARGET}.exe")
  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set_target_properties(${PRODUCER_TARGET} PROPERTIES
      VS_DEBUGGER_COMMAND "${PRODUCER_OUTPUT_DIR}/${PRODUCER_TARGET}.exe")
  endif()
else()
  add_executable(${PRODUCER_TARGET} WIN32 ${PRODUCER_SOURCES})
  SET_EXECUTABLE_TARGET_PROPERTIES(${PRODUCER_TARGET})
endif()

set_target_properties(${PRODUCER_TARGET} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PRODUCER_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${PRODUCER_OUTPUT_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIGURATION>")

target_compile_features(${PRODUCER_TARGET} PRIVATE cxx_std_20)
target_include_directories(${PRODUCER_TARGET} PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(${PRODUCER_TARGET} PRIVATE
  streaming_common
  libcef_lib
  libcef_dll_wrapper
  ${CEF_STANDARD_LIBS}
  d3d11.lib
  dxgi.lib
  shell32.lib)
add_dependencies(${PRODUCER_TARGET} libcef_dll_wrapper)

COPY_FILES("${PRODUCER_TARGET}" "${CEF_BINARY_FILES}"
  "${CEF_BINARY_DIR}" "${PRODUCER_OUTPUT_DIR}")
COPY_FILES("${PRODUCER_TARGET}" "${CEF_RESOURCE_FILES}"
  "${CEF_RESOURCE_DIR}" "${PRODUCER_OUTPUT_DIR}")
ADD_WINDOWS_MANIFEST("${CMAKE_CURRENT_SOURCE_DIR}/win" "${PRODUCER_TARGET}" "exe")

if(USE_SANDBOX)
  SET_LPAC_ACLS("${PRODUCER_TARGET}")
endif()
