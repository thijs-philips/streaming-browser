cmake_minimum_required(VERSION 3.21)

set(CMAKE_CONFIGURATION_TYPES Debug Release CACHE STRING "" FORCE)
project(streaming_browser LANGUAGES CXX VERSION 0.1.0)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "streaming-browser currently supports Windows x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(FetchContent)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG f7320141120f720aecc4c32be25586e7da9eb978)
FetchContent_MakeAvailable(yaml-cpp)

set(CEF_VERSION "150.0.14+g7c1aa68+chromium-150.0.7871.129" CACHE STRING "Pinned CEF version")
set(CEF_PLATFORM "windows64")
set(CEF_ARCHIVE_SHA1 "626fb887a04e9c5668f13eae8a39c37880cb818e")
set(api_version "15000" CACHE STRING "Pinned CEF API version")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DownloadCEF)
DownloadPinnedCEF(
  "${CEF_PLATFORM}"
  "${CEF_VERSION}"
  "${CEF_ARCHIVE_SHA1}"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

option(STREAMING_BROWSER_BUILD_TESTS "Build tests" ON)
add_subdirectory(src)

if(STREAMING_BROWSER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

PRINT_CEF_CONFIG()
