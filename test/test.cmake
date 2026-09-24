include(CTest)

add_library(test-lib-emutest ${CMAKE_CURRENT_SOURCE_DIR}/../test-lib-emutest.c)
target_include_directories(test-lib-emutest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../)

function(add_emutest_test name binext source_dir libretro_core)
  add_executable(${name}.${binext} ${source_dir}/${name}.c)
  target_link_libraries(${name}.${binext} test-lib-emutest)
  get_property(libretro_shared_lib VARIABLE PROPERTY ${libretro_core})
  add_test(NAME test-${name} COMMAND ${EMUTEST_COMMAND} -T
    -L ${libretro_shared_lib}
    -r $<TARGET_FILE:${name}.${binext}>
    -t ${CMAKE_CURRENT_SOURCE_DIR}/../emutest.lua)
endfunction()

function(add_common_compile_test target type)
  add_executable(${target} ${target}.c)
  add_test(NAME ${target}-${type} COMMAND ${CMAKE_CTEST_COMMAND}
    --build-and-test ${CMAKE_CURRENT_SOURCE_DIR}/..
                     ${CMAKE_CURRENT_BINARY_DIR}/${target}
    --build-generator ${CMAKE_GENERATOR}
    --build-makeprogram ${CMAKE_MAKE_PROGRAM}
    --build-target ${target}
    --build-options
      -DLLVM_MOS=${LLVM_MOS}
      -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
      -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
      -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
      -DCMAKE_EXPORT_COMPILE_COMMANDS=${CMAKE_EXPORT_COMPILE_COMMANDS}
    )
endfunction()

# negative (failure) compilation test
function(add_compile_test target)
  add_common_compile_test(${target} compile)
endfunction()

# negative (failure) compilation test
function(add_no_compile_test target)
  add_common_compile_test(${target} no-compile)
  set_property(TEST ${target}-no-compile PROPERTY WILL_FAIL YES)
endfunction()

# Emulator test for the C128. Results are reported the same way as for the other
# emutest platforms: the program returns EXIT_SUCCESS or EXIT_FAILURE (see
# README.md). The program is always built; up to two CTest tests run it:
#   test-<name>          under VICE's x128 via vice-runner.py, when
#                        VICE_X128_COMMAND and Python are available; also checks
#                        that the Common-RAM code area is restored at exit
#   test-<name>-libretro under emutest with the libretro VICE x128 core, when
#                        EMUTEST_COMMAND and LIBRETRO_VICE_X128_CORE are found
#   name          - target/test name; the source is <name>.c unless SOURCE is given
#   SOURCE        - source file, to build one source with different link options
#   LINK_OPTIONS  - extra link options
#   RESTORE_RANGE - hex start-end of the Common-RAM code area the runner checks
#                   is restored at exit (default: the platform default)
function(add_vice_test name)
  cmake_parse_arguments(ARG "" "SOURCE;RESTORE_RANGE" "LINK_OPTIONS" ${ARGN})
  if(NOT ARG_SOURCE)
    set(ARG_SOURCE ${name}.c)
  endif()
  add_executable(${name}.prg ${ARG_SOURCE})
  target_link_libraries(${name}.prg test-lib-emutest)
  # -u: LTO would otherwise delete test_result, which the program only writes
  # and the runner reads from outside.
  target_link_options(${name}.prg PRIVATE
    -Wl,-Map=$<TARGET_FILE:${name}.prg>.map -Wl,-u,test_result ${ARG_LINK_OPTIONS})
  find_program(VICE_TEST_PYTHON NAMES python python3)
  if(VICE_X128_COMMAND AND VICE_TEST_PYTHON)
    set(restore_args "")
    if(ARG_RESTORE_RANGE)
      set(restore_args --restore-range ${ARG_RESTORE_RANGE})
    endif()
    add_test(NAME test-${name} COMMAND ${VICE_TEST_PYTHON}
      ${CMAKE_CURRENT_SOURCE_DIR}/../vice-runner.py
      --vice ${VICE_X128_COMMAND}
      --prg $<TARGET_FILE:${name}.prg>
      --map $<TARGET_FILE:${name}.prg>.map
      ${restore_args})
    # One emulator at a time: instances share VICE's settings and audio.
    set_tests_properties(test-${name} PROPERTIES
      RESOURCE_LOCK vice TIMEOUT 180)
  endif()
  if(EMUTEST_COMMAND AND LIBRETRO_VICE_X128_CORE)
    add_test(NAME test-${name}-libretro COMMAND ${EMUTEST_COMMAND} -T
      -L ${LIBRETRO_VICE_X128_CORE}
      -r $<TARGET_FILE:${name}.prg>
      -t ${CMAKE_CURRENT_SOURCE_DIR}/../emutest.lua)
  endif()
endfunction()

function(add_vcs_test name)
  set(source_dir ".")
  if(ARGC GREATER 1)
    set(source_dir ${ARGV1})
  endif()
  add_emutest_test(${name} a26 ${source_dir} LIBRETRO_STELLA_CORE)
endfunction()

function(add_nes_test name)
  set(source_dir ".")
  if(ARGC GREATER 1)
    set(source_dir ${ARGV1})
  endif()
  add_emutest_test(${name} nes ${source_dir} LIBRETRO_MESEN_CORE)
endfunction()
