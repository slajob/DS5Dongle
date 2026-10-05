cmake_minimum_required(VERSION 3.16)
project(ds5bridged LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(DS5BRIDGE_VERSION "0.1.0" CACHE STRING "Version reported by the daemon")
option(DS5BRIDGE_STATIC_RUNTIME "Link libstdc++/libgcc statically (portable binary)" ON)

# Opus: use the firmware's submodule (same codec version as the Pico) when the
# plugin is built inside the DS5Dongle repository, otherwise fetch that commit.
set(DS5BRIDGE_OPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/opus" CACHE PATH "Opus source tree")
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
if(EXISTS "${DS5BRIDGE_OPUS_DIR}/CMakeLists.txt")
  add_subdirectory("${DS5BRIDGE_OPUS_DIR}" opus EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG 2d862ea14b233e5a3f3afaf74d96050691af3cd5)
  FetchContent_GetProperties(opus)
  if(NOT opus_POPULATED)
    FetchContent_Populate(opus)
    add_subdirectory(${opus_SOURCE_DIR} ${opus_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()
endif()

find_package(Threads REQUIRED)

add_library(ds5bridge_core STATIC
  src/audio.cpp
  src/bridge.cpp
  src/bt_link.cpp
  src/config.cpp
  src/feature_worker.cpp
  src/usb_descriptors.cpp
  src/usbip.cpp
  src/virtual_device.cpp)
target_include_directories(ds5bridge_core PUBLIC src)
target_compile_options(ds5bridge_core PRIVATE -Wall -Wextra)
target_compile_definitions(ds5bridge_core PUBLIC DS5BRIDGE_VERSION="${DS5BRIDGE_VERSION}")
target_link_libraries(ds5bridge_core PUBLIC opus Threads::Threads)

add_executable(ds5bridged src/main.cpp)
target_compile_options(ds5bridged PRIVATE -Wall -Wextra)
target_link_libraries(ds5bridged PRIVATE ds5bridge_core)

# Test helper: encodes PCM into the 71-byte Opus mic frames the controller sends.
add_executable(ds5bridge_testtool tests/testtool.cpp)
target_link_libraries(ds5bridge_testtool PRIVATE opus)

if(DS5BRIDGE_STATIC_RUNTIME)
  target_link_options(ds5bridged PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS ds5bridged RUNTIME DESTINATION bin)
