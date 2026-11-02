cmake_minimum_required(VERSION 3.29)
project(cinc VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${PROJECT_IS_TOP_LEVEL})

option(CINC_BUILD_EXAMPLES "compile the cinc examples" ${PROJECT_IS_TOP_LEVEL})

# a parent project may already provide cilo, otherwise it is fetched.
include(FetchContent)
if(NOT TARGET cilo::cilo)
  FetchContent_Declare(cilo GIT_REPOSITORY ssh://git@vexcited.com/ciko/cilo.git GIT_TAG main)
  FetchContent_MakeAvailable(cilo)
endif()

# headers only until the runtime exists.
add_library(cinc INTERFACE)
add_library(cinc::cinc ALIAS cinc)
target_include_directories(cinc INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(cinc INTERFACE cilo::cilo)

if(CINC_BUILD_EXAMPLES)
  # object libraries: nothing links yet, building them checks that the api compiles.
  add_library(cinc_examples OBJECT examples/01_tasks.c examples/02_channels.c examples/03_colorless.c)
  target_link_libraries(cinc_examples PRIVATE cinc)
  target_compile_options(cinc_examples PRIVATE -Wall -Wextra -Wpedantic)
endif()
