cmake_minimum_required(VERSION 3.28)
project(Helix.Profiler LANGUAGES CXX)

if(NOT COMMAND helix_add_library)
    include("${CMAKE_CURRENT_SOURCE_DIR}/external/Helix.CMake/Helix.cmake")
endif()

helix_require(Core)
helix_require(Platform)
helix_add_library(helix_profiler ALIAS Profiler PUBLIC_DEPS Helix::Core Helix::Platform)

helix_add_test_suite(helix_profiler_tests DEPS Helix::Profiler)
helix_add_benchmark_suite(helix_profiler_bench DEPS Helix::Profiler)
