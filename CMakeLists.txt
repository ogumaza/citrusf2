# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# citrusf2 --version reports this version. The file properties of the Windows program and the Info.plist of the macOS
# app show it too. A release changes it here and adds its notes in docs/release-notes/.
project(citrusf2 VERSION 1.0.4 LANGUAGES CXX)

foreach(submodule seqlib xsflib ntdlib)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/${submodule}/CMakeLists.txt")
    message(FATAL_ERROR "externals/${submodule} is missing. Run: git submodule update --init")
  endif()
endforeach()

# The C++ standard, a release build by default, and the compiler options that keep the output the same on every
# platform, as xsflib sets them for the converters.
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(WARNINGS_ON_EVERY_TARGET)

# The submodules: seqlib writes the MIDI, SoundFont and SFZ files. ntdlib holds the Nintendo code that citrusf2 shares
# with the other converters and the players: the archive reader, the performer, the conversion and the command-line
# frame (ntdlib::sequence_export). xsflib holds the common code that ntdlib builds on, and the CMake helpers.
set(NTDLIB_SEQUENCE_EXPORT ON)
xsflib_add_libraries(seqlib xsflib ntdlib)

# The libraries that citrusf2 converts with. ntdlib's sequence_export/consoles.h holds the 3DS's settings.
add_library(citrusf2_core INTERFACE)
target_link_libraries(citrusf2_core INTERFACE seqlib::seqlib ntdlib::sar ntdlib::sequence_export)

add_executable(citrusf2 src/main.cpp)
target_link_libraries(citrusf2 PRIVATE citrusf2_core)
xsflib_add_version(citrusf2 VERSION ${PROJECT_VERSION} DEFINE CITRUSF2_VERSION
                   DESCRIPTION "Nintendo 3DS sound archive converter" PRODUCT citrusf2
                   COPYRIGHT "Copyright (c) 2026 the citrusf2 authors. MIT licence.")

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

  # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
  xsflib_include_local_tests()
endif()
