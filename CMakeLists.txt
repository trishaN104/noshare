cmake_minimum_required(VERSION 3.16)
project(lockfree_spsc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Header-only library target.
add_library(lfq INTERFACE)
target_include_directories(lfq INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)

if(MSVC)
  set(LFQ_WARN /W4 /O2 /wd4324)
else()
  set(LFQ_WARN -Wall -Wextra -O2)
endif()

# Optional sanitizer build: -DLFQ_SANITIZE=address|thread|undefined
function(lfq_add exe src)
  add_executable(${exe} ${src})
  target_link_libraries(${exe} PRIVATE lfq Threads::Threads)
  target_compile_options(${exe} PRIVATE ${LFQ_WARN})
  if(LFQ_SANITIZE AND NOT MSVC)
    target_compile_options(${exe} PRIVATE -fsanitize=${LFQ_SANITIZE} -g)
    target_link_options(${exe} PRIVATE -fsanitize=${LFQ_SANITIZE})
  endif()
endfunction()

lfq_add(test_spsc tests/test_spsc.cpp)
lfq_add(test_mpsc tests/test_mpsc.cpp)
lfq_add(test_mpmc tests/test_mpmc.cpp)
lfq_add(test_shm tests/test_shm.cpp)
lfq_add(test_reference tests/test_reference.cpp)
lfq_add(test_histogram tests/test_histogram.cpp)
lfq_add(test_timing tests/test_timing.cpp)
lfq_add(bench_spsc bench/bench_spsc.cpp)
lfq_add(bench_compare bench/bench_compare.cpp)
lfq_add(bench_csv bench/bench_csv.cpp)
lfq_add(scope tools/latency_scope/scope.cpp)

# The POSIX shared-memory backend needs librt on Linux.
if(UNIX AND NOT APPLE)
  target_link_libraries(test_shm PRIVATE rt)
endif()

enable_testing()
add_test(NAME spsc_correctness COMMAND test_spsc)
add_test(NAME mpsc_correctness COMMAND test_mpsc)
add_test(NAME mpmc_correctness COMMAND test_mpmc)
add_test(NAME shm_smoke COMMAND test_shm)
add_test(NAME reference_validation COMMAND test_reference)
add_test(NAME histogram_tests COMMAND test_histogram)
add_test(NAME timing_tests COMMAND test_timing)
