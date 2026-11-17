# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)
project(sharc VERSION 0.1.0 LANGUAGES CXX)

find_package(LLVM 19.1 REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 19)
  message(FATAL_ERROR "SHARC requires LLVM 19")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SHARC_POISON_NATIVE_STATE "Check stale pinned fields during native execution" OFF)

# The preparation identity follows source contents, including uncommitted edits.
set(identity_input "${LLVM_PACKAGE_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_SIZEOF_VOID_P};${SHARC_POISON_NATIVE_STATE}")
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

add_library(sharc_core src/core.cpp src/decode.cpp src/compute.cpp src/image.cpp src/lower.cpp src/memory.cpp src/abi.cpp)
add_library(sharc::core ALIAS sharc_core)
if(SHARC_POISON_NATIVE_STATE)
  target_compile_definitions(sharc_core PRIVATE SHARC_POISON_NATIVE_STATE=1)
endif()
set_target_properties(sharc_core PROPERTIES EXPORT_NAME core)
target_compile_features(sharc_core PUBLIC cxx_std_23)
target_include_directories(sharc_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(sharc_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
llvm_map_components_to_libnames(llvm_libraries Support Core BitReader BitWriter OrcJIT native nativecodegen Passes Object DebugInfoDWARF)
target_link_libraries(sharc_core PRIVATE ${llvm_libraries})
target_compile_options(sharc_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

install(TARGETS sharc_core EXPORT sharcTargets)
install(FILES include/sharc/core.hpp include/sharc/memory.hpp
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sharc")
install(FILES LICENSE THIRD_PARTY.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/sharc")
set(package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/sharc")
configure_package_config_file(cmake/sharcConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/sharcConfig.cmake" INSTALL_DESTINATION "${package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/sharcConfigVersion.cmake"
  COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/sharcConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/sharcConfigVersion.cmake" DESTINATION "${package_dir}")
install(EXPORT sharcTargets NAMESPACE sharc:: DESTINATION "${package_dir}")

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  file(GLOB reference_sources CONFIGURE_DEPENDS test/support/*.py)
  set(fixed_expectations "${CMAKE_CURRENT_BINARY_DIR}/generated/fixed_cases.hpp")
  add_custom_command(OUTPUT "${fixed_expectations}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/support/generate_expectations.py"
      --fixed-header "${fixed_expectations}"
    DEPENDS ${reference_sources}
    VERBATIM)
  add_executable(sharc_isa_test test/isa/fixed.cpp test/isa/control.cpp test/isa/indirect.cpp test/isa/register.cpp test/isa/rotate.cpp test/isa/memory.cpp test/isa/dag.cpp "${fixed_expectations}")
  target_link_libraries(sharc_isa_test PRIVATE sharc::core)
  target_include_directories(sharc_isa_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_compile_options(sharc_isa_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
  add_test(NAME isa COMMAND sharc_isa_test)
  foreach(suite IN ITEMS preparation state compute native control memory faultmap)
    add_executable(sharc_${suite}_test test/core/${suite}.cpp)
    target_link_libraries(sharc_${suite}_test PRIVATE sharc::core)
    target_compile_options(sharc_${suite}_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME ${suite} COMMAND sharc_${suite}_test)
  endforeach()
  target_include_directories(sharc_native_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(sharc_native_test PRIVATE ${llvm_libraries})
  target_include_directories(sharc_faultmap_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  target_link_libraries(sharc_faultmap_test PRIVATE ${llvm_libraries})
  add_test(NAME installed_consumer COMMAND "${CMAKE_COMMAND}"
    "-Dcore_build=${CMAKE_CURRENT_BINARY_DIR}"
    "-Dconsumer_source=${CMAKE_CURRENT_SOURCE_DIR}/test/core/consumer"
    "-Dconsumer_build=${CMAKE_CURRENT_BINARY_DIR}/local/consumer"
    "-Dllvm_config=${LLVM_DIR}" "-Dcompiler=${CMAKE_CXX_COMPILER}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/test/core/install.cmake")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_executable(sharc_k5_bench test/bench/k5.cpp test/bench/reference.cpp)
    target_link_libraries(sharc_k5_bench PRIVATE sharc::core)
    target_compile_options(sharc_k5_bench PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    set_source_files_properties(test/bench/reference.cpp PROPERTIES COMPILE_OPTIONS "-O3;-fno-lto")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
      set_property(SOURCE test/bench/reference.cpp APPEND PROPERTY COMPILE_OPTIONS -march=native)
    endif()
    add_test(NAME bench_k5 COMMAND sharc_k5_bench --check)
    add_executable(sharc_k4_bench test/bench/k4.cpp test/bench/reference.cpp)
    target_link_libraries(sharc_k4_bench PRIVATE sharc::core)
    target_compile_options(sharc_k4_bench PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME bench_k4 COMMAND sharc_k4_bench --check)
  endif()

endif()
