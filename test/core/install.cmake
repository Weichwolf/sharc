# SPDX-License-Identifier: GPL-3.0-only
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Installed consumer command failed: ${ARGV}")
  endif()
endfunction()

run("${CMAKE_COMMAND}" --install "${core_build}" --prefix "${consumer_build}/prefix")
run("${CMAKE_COMMAND}" -S "${consumer_source}" -B "${consumer_build}/build" -G Ninja
    "-DCMAKE_PREFIX_PATH=${consumer_build}/prefix" "-DLLVM_DIR=${llvm_config}"
    "-DCMAKE_CXX_COMPILER=${compiler}" -DCMAKE_BUILD_TYPE=Release)
run("${CMAKE_COMMAND}" --build "${consumer_build}/build")
run("${consumer_build}/build/consumer")
