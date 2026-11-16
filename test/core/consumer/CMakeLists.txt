# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.25)
project(sharc_consumer LANGUAGES CXX)
find_package(sharc REQUIRED CONFIG)
add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE sharc::core)
target_compile_features(consumer PRIVATE cxx_std_23)
target_compile_options(consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
