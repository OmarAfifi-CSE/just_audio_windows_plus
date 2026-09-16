cmake_minimum_required(VERSION 3.14)
set(PROJECT_NAME "just_audio_windows_plus")
project(${PROJECT_NAME} LANGUAGES CXX)
# C++20, not 17, because of a header C++/WinRT chooses on our behalf. Built as
# C++17 it includes <experimental/coroutine>; built as C++20 it includes the
# standard <coroutine>. MSVC 14.51, which ships with Visual Studio 2026, turned
# the deprecation of the experimental header into a hard error:
#
#   error C2338: static assertion failed: 'error STL1011: The /await compiler
#   option, <experimental/coroutine> ... are deprecated by Microsoft and will be
#   REMOVED SOON.'
#
# So this plugin requires C++20 under VS 2026 and modern MSVC compilers.
set(CMAKE_CXX_STANDARD 20)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "just_audio_windows_plus_plugin")

add_library(${PLUGIN_NAME} SHARED
  "just_audio_windows_plus_plugin.cpp"
  "platform_thread.hpp"
  "player.hpp"
  "native_utils.hpp"
  "source_model.hpp"
)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin)

# List of absolute paths to libraries that should be bundled with the plugin
set(just_audio_windows_plus_bundled_libraries
  ""
  PARENT_SCOPE
)

# === Tests ===

option(include_just_audio_windows_plus_tests "Build native audio unit tests" OFF)
if (include_just_audio_windows_plus_tests)
  set(TEST_RUNNER "${PROJECT_NAME}_test")
  enable_testing()

  # Download googletest
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
  )
  # Prevent overriding the parent project's compiler/linker settings.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  # Disable install commands for gtest so it doesn't end up in the bundle.
  set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)

  FetchContent_MakeAvailable(googletest)

  # Exercise production parsing and the actual Win32 dispatcher, not a mock
  # implementation of their algorithms. Playback tests use the Flutter runner.
  add_executable(${TEST_RUNNER}
    test/uri_utils_test.cpp
    test/source_model_test.cpp
    test/platform_thread_test.cpp
  )
  apply_standard_settings(${TEST_RUNNER})
  # source_model.hpp includes flutter/encodable_value.h, which lives in the
  # cpp_client_wrapper include directory owned by the wrapper target, not the
  # `flutter` target, so both INTERFACE_INCLUDE_DIRECTORIES are needed.
  target_include_directories(${TEST_RUNNER} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "$<TARGET_PROPERTY:flutter,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:flutter_wrapper_plugin,INTERFACE_INCLUDE_DIRECTORIES>")
  target_link_libraries(${TEST_RUNNER} PRIVATE gtest_main user32)

  include(GoogleTest)
  gtest_discover_tests(${TEST_RUNNER} DISCOVERY_TIMEOUT 30)
endif()
