cmake_minimum_required(VERSION 3.29)

# c2y and the defer TS take clang 22, newer than the one Xcode ships.
if(APPLE AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
  find_program(CMAKE_C_COMPILER clang
    PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
endif()

project(cinc VERSION 0.1.0 LANGUAGES C ASM)

# cmake knows no standard past C23, so the flags are given by hand.
set(CINC_C2Y -std=c2y -fdefer-ts)
include(CheckCCompilerFlag)
check_c_compiler_flag(-fdefer-ts CINC_HAS_DEFER)
if(NOT CINC_HAS_DEFER)
  message(FATAL_ERROR
    "cinc is C2y with the defer TS, set CC to clang 22 or later")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "build type" FORCE)
endif()

option(CINC_BUILD_TESTS "build the cinc tests and benchmarks" ${PROJECT_IS_TOP_LEVEL})
option(CINC_BUILD_EXAMPLES "build the cinc examples" ${PROJECT_IS_TOP_LEVEL})
set(CINC_SANITIZE "" CACHE STRING "sanitizers for the whole build, cilo included: address;undefined")

# set before cilo is added, so its arenas see asan too.
if(CINC_SANITIZE)
  list(JOIN CINC_SANITIZE "," sanitizers)
  add_compile_options(-fsanitize=${sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${sanitizers})
endif()

# a parent project may already provide cilo, otherwise it is fetched,
# from a checkout next to this one when there is.
include(FetchContent)
get_filename_component(local ${CMAKE_CURRENT_SOURCE_DIR}/../ciko-cilo ABSOLUTE)
if(NOT FETCHCONTENT_SOURCE_DIR_CILO AND EXISTS ${local}/CMakeLists.txt)
  set(FETCHCONTENT_SOURCE_DIR_CILO ${local})
endif()
if(NOT TARGET cilo::cilo)
  FetchContent_Declare(cilo GIT_REPOSITORY https://code.vexcited.com/ciko/cilo.git GIT_TAG main)
  FetchContent_MakeAvailable(cilo)
endif()

find_package(Threads REQUIRED)
# both switches are always built, each one empty off its architecture, so universal builds work too.
add_library(cinc STATIC src/runtime.c src/sync.c src/poll.c src/net.c src/switch_aarch64.S src/switch_x86_64.S)
add_library(cinc::cinc ALIAS cinc)
target_include_directories(cinc PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(cinc PUBLIC cilo::cilo Threads::Threads)
target_compile_options(cinc PRIVATE $<$<COMPILE_LANGUAGE:C>:${CINC_C2Y} -Wall -Wextra -Wpedantic>)
set_target_properties(cinc PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CINC_BUILD_TESTS)
  enable_testing()
  foreach(name tasks blocks deadlines channels select mutex semaphore cond blocking tcp sleep stacks scratch plain shutdown)
    add_executable(test_${name} tests/${name}.c)
    target_link_libraries(test_${name} PRIVATE cinc)
    target_compile_options(test_${name} PRIVATE ${CINC_C2Y} -Wall -Wextra -Wpedantic)
    target_compile_definitions(test_${name} PRIVATE _GNU_SOURCE _DARWIN_C_SOURCE)
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
  endforeach()
  add_executable(cinc_bench bench/bench.c)
  target_link_libraries(cinc_bench PRIVATE cinc)
  target_compile_options(cinc_bench PRIVATE ${CINC_C2Y} -O2 -Wall -Wextra -Wpedantic)
  target_compile_definitions(cinc_bench PRIVATE _GNU_SOURCE _DARWIN_C_SOURCE)
endif()

if(CINC_BUILD_EXAMPLES)
  foreach(name 01_tasks 02_channels 03_colorless)
    add_executable(${name} examples/${name}.c)
    target_link_libraries(${name} PRIVATE cinc)
    target_compile_options(${name} PRIVATE ${CINC_C2Y} -Wall -Wextra -Wpedantic)
    if(CINC_BUILD_TESTS)
      add_test(NAME example_${name} COMMAND ${name})
      set_tests_properties(example_${name} PROPERTIES TIMEOUT 60)
    endif()
  endforeach()
endif()

