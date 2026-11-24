# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# citrusf2 --version reports this version. The file properties of the Windows program and the Info.plist of the macOS
# app show it too. A release changes it here and adds its notes in docs/release-notes/.
project(citrusf2 VERSION 1.1.2 LANGUAGES CXX)

# The submodules: seqlib writes the MIDI, SoundFont and SFZ files. ntdlib contains the Nintendo code that citrusf2
# shares with the other converters and the players: the archive reader, the performer, the conversion and the
# command-line frame (ntdlib::sequence_export). xsflib contains the common code that ntdlib builds on, and the CMake
# helpers. xsflib's XsflibProject.cmake sets the C++ standard, a release build by default, the static C runtime and the
# compiler options that keep the output the same on every platform. ntdlib builds on seqlib and xsflib. xsflib_project
# adds the libraries in that order, after it turns on NTDLIB_SEQUENCE_EXPORT, ntdlib's conversion of sequences. It stops
# if a submodule is missing. The check below covers xsflib itself. xsflib_project also turns off clang-cl's
# -Wmissing-designated-field-initializers for every target, the libraries too. A designated initializer names only the
# fields that differ from their defaults.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
  message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(WARNINGS_ON_EVERY_TARGET ENABLE NTDLIB_SEQUENCE_EXPORT LIBRARIES seqlib xsflib ntdlib)

# The libraries that citrusf2 and its tests link. ntdlib's sequence_export/consoles.h contains the 3DS settings.
# ntdlib::ctr_starting_tracks contains the game profiles.
add_library(citrusf2_core INTERFACE)
target_link_libraries(citrusf2_core INTERFACE seqlib::seqlib ntdlib::sar ntdlib::sequence_export
                      ntdlib::ctr_starting_tracks)

# The games' profiles (src/profiles.h). The program and its unit test link them. ntdlib::ctr_game_effects contains the
# effects that the games put on the aux buses.
add_library(citrusf2_profiles STATIC src/profiles.cpp)
target_include_directories(citrusf2_profiles PUBLIC src)
target_link_libraries(citrusf2_profiles PUBLIC citrusf2_core PRIVATE ntdlib::ctr_game_effects)

add_executable(citrusf2 src/main.cpp)
target_link_libraries(citrusf2 PRIVATE citrusf2_profiles)
xsflib_add_version(citrusf2 VERSION ${PROJECT_VERSION} DEFINE CITRUSF2_VERSION
                   DESCRIPTION "Nintendo 3DS sound archive converter" PRODUCT citrusf2
                   COPYRIGHT "Copyright (c) 2026 the citrusf2 authors. MIT licence.")

# Windows: UTF-8 as the code page. An archive or a folder whose name the system's code page can't represent then
# opens.
xsflib_add_utf8_manifest(citrusf2)

# Finder cannot drop files onto a command-line executable. Package it in a macOS droplet app, with the version in its
# Info.plist.
xsflib_add_droplet_tool(citrusf2 APP_NAME Citrusf2 VERSION ${PROJECT_VERSION}
                        DESCRIPTION "Drop 3DS sound archives (.bcsar) here to convert them to MIDI, SoundFont and SFZ. Output goes in a folder beside each archive."
                        PROMPT "Choose 3DS sound archives (.bcsar) to convert:" TRUNCATED)

include(CTest)
if(BUILD_TESTING)
  # The program reports its version, and it rejects --var 32=1. -DCITRUSF2_TEST_ARCHIVE=path/to/archive.bcsar also
  # converts and checks every sequence of an archive with tests/citrusf2_test.cpp.
  xsflib_add_converter_tests(citrusf2 EXTENSION .bcsar VERSION ${PROJECT_VERSION}
                             LIBRARIES citrusf2_core ntdlib::testing)

  # The unit test of the games' profiles: the reverbs on bus A that decide where send A goes out as CC91, and the
  # start of a state with a low-pass.
  add_executable(citrusf2_profiles_test tests/profiles_test.cpp)
  target_link_libraries(citrusf2_profiles_test PRIVATE citrusf2_profiles xsflib::testing)
  add_test(NAME citrusf2_profiles COMMAND citrusf2_profiles_test)

  # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
  xsflib_include_local_tests()
endif()
