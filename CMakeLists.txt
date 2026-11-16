# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)
project(sharc VERSION 0.1.0 LANGUAGES CXX)

find_package(LLVM 19.1 REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 19)
  message(FATAL_ERROR "SHARC requires LLVM 19")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The preparation identity follows source contents, including uncommitted edits.
set(identity_input "${LLVM_PACKAGE_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION}")
file(GLOB_RECURSE identity_sources CONFIGURE_DEPENDS src/*.cpp src/*.hpp include/*.hpp)
list(APPEND identity_sources "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
foreach(source IN LISTS identity_sources)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
  file(SHA256 "${source}" source_hash)
  string(APPEND identity_input ";${source_hash}")
endforeach()
string(SHA256 compiler_hash "${identity_input}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/compiler_identity.hpp"
  CONTENT "// SPDX-License-Identifier: GPL-3.0-only\n#pragma once\n#define SHARC_COMPILER_IDENTITY \"sharc-preparation-v1-llvm${LLVM_PACKAGE_VERSION}-${compiler_hash}\"\n")

add_library(sharc_core src/core.cpp src/decode.cpp)
add_library(sharc::core ALIAS sharc_core)
target_include_directories(sharc_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(sharc_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
llvm_map_components_to_libnames(llvm_libraries Support)
target_link_libraries(sharc_core PRIVATE ${llvm_libraries})
target_compile_options(sharc_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

include(CTest)
if(BUILD_TESTING)
  foreach(suite IN ITEMS preparation state)
    add_executable(sharc_${suite}_test test/core/${suite}.cpp)
    target_link_libraries(sharc_${suite}_test PRIVATE sharc::core)
    target_compile_options(sharc_${suite}_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME ${suite} COMMAND sharc_${suite}_test)
  endforeach()
endif()
