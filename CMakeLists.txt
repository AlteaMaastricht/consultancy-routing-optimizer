cmake_minimum_required(VERSION 3.16)
project(consultancy_routing_optimizer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The original header comment compiles with -march=native. That makes the binary
# non-portable (it may use CPU instructions other machines lack), so it is opt-in here.
option(SA_NATIVE "Compile with -march=native (non-portable binary)" OFF)
# Debug aid: AddressSanitizer + UndefinedBehaviorSanitizer (use with RelWithDebInfo).
option(SA_SANITIZE "Build with -fsanitize=address,undefined" OFF)

find_package(Threads REQUIRED)

add_executable(sa cpp/sa.cpp)
target_link_libraries(sa PRIVATE Threads::Threads)
target_compile_options(sa PRIVATE $<$<CONFIG:Release>:-O3>)
if(SA_NATIVE)
  target_compile_options(sa PRIVATE -march=native)
endif()
if(SA_SANITIZE)
  target_compile_options(sa PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(sa PRIVATE -fsanitize=address,undefined)
endif()
