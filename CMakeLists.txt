cmake_minimum_required(VERSION 3.16)
project(lsm_tree CXX)

if(NOT APPLE)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    # Default to Release without overriding an explicit choice.
    #
    # A bare set() clobbered whatever the developer passed on the command line, so
    # -DCMAKE_BUILD_TYPE=Debug silently produced a Release build and the sanitizer
    # and debug workflows the tests rely on could not be selected at all.
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # CMAKE_CXX_FLAGS_RELEASE is left at CMake's default, which is "-O3 -DNDEBUG".
    #
    # It was previously set to "-O3" alone. That is the same optimisation level but
    # drops -DNDEBUG, so assert() stays live in Release builds — the release binary
    # keeps paying for checks it is meant to have compiled out, and an assertion
    # failure aborts a production process rather than being absent.

    # Find system libraries
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(URING REQUIRED liburing)
    find_package(Threads REQUIRED)

    # Core library target containing all modular database components
    add_library(lsm_core STATIC
        src/wal/wal_writer.cpp
        src/wal/wal_reader.cpp
        src/sstable/sstable_builder.cpp
        src/sstable/sstable_reader.cpp
        src/compaction/leveled_compactor.cpp
        src/db/lsm_engine.cpp
    )
    target_include_directories(lsm_core PUBLIC src)
    target_link_libraries(lsm_core PUBLIC ${URING_LIBRARIES} Threads::Threads)

    # Unit Tests targets
    add_executable(test_skip_list tests/test_skip_list.cpp)
    target_link_libraries(test_skip_list PRIVATE lsm_core)

    add_executable(test_bloom_filter tests/test_bloom_filter.cpp)
    target_link_libraries(test_bloom_filter PRIVATE lsm_core)

    add_executable(test_wal_recovery tests/test_wal_recovery.cpp)
    target_link_libraries(test_wal_recovery PRIVATE lsm_core)

    add_executable(test_sstable tests/test_sstable.cpp)
    target_link_libraries(test_sstable PRIVATE lsm_core)

    # Benchmark targets
    add_executable(bench_write benchmarks/bench_write.cpp)
    target_link_libraries(bench_write PRIVATE lsm_core)

    add_executable(bench_read benchmarks/bench_read.cpp)
    target_link_libraries(bench_read PRIVATE lsm_core)

    add_executable(bench_block_bottleneck benchmarks/bench_block_bottleneck.cpp)
    target_link_libraries(bench_block_bottleneck PRIVATE lsm_core)

    # Interactive CLI Tool
    add_executable(lsm_cli tools/lsm_cli.cpp)
    target_link_libraries(lsm_cli PRIVATE lsm_core)

else()
    message(FATAL_ERROR "This project requires Linux for native io_uring and O_DIRECT support.")
endif()
