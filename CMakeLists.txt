cmake_minimum_required(VERSION 3.20)
project(graphvec CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# -O2 plus frame pointers so perf call graphs work; x86-64-v3 = AVX2 + FMA,
# the same ISA level for the local laptop and the CI runners.
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
add_compile_options(-Wall -Wextra -fno-omit-frame-pointer)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-march=x86-64-v3)
endif()

find_package(Threads REQUIRED)

add_library(graphvec
  src/value.cpp src/graph.cpp src/store.cpp src/dataset.cpp
  src/cypher_parser.cpp src/cypher_exec.cpp
  src/net.cpp src/etcd.cpp)
target_include_directories(graphvec PUBLIC include)
target_link_libraries(graphvec PUBLIC Threads::Threads)

foreach(t gvsh filter_bench wal_crash shardd chaos_client)
  add_executable(${t} tools/${t}.cpp)
  target_link_libraries(${t} graphvec)
endforeach()

# ann_bench links hnswlib (header-only) so both indexes are timed by the
# same harness, compiler and flags.
include(FetchContent)
FetchContent_Declare(hnswlib
  URL https://github.com/nmslib/hnswlib/archive/refs/tags/v0.8.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(hnswlib)
if(NOT hnswlib_POPULATED)
  FetchContent_Populate(hnswlib)
endif()
add_executable(ann_bench tools/ann_bench.cpp)
target_include_directories(ann_bench SYSTEM PRIVATE ${hnswlib_SOURCE_DIR})
target_link_libraries(ann_bench graphvec)

enable_testing()
add_executable(unit_tests tests/test_main.cpp)
target_link_libraries(unit_tests graphvec)
add_test(NAME unit_tests COMMAND unit_tests)
