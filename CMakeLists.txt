cmake_minimum_required(VERSION 3.16)
project(lsm_tree CXX)

if(NOT APPLE)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_BUILD_TYPE Release)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2")

    # Required for io_uring
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(URING REQUIRED liburing)

    add_executable(lsm_tests tests/main.cpp)
    target_include_directories(lsm_tests PRIVATE include)
    target_link_libraries(lsm_tests PRIVATE ${URING_LIBRARIES} pthread)
else()
    message(FATAL_ERROR "This project now requires Linux for native io_uring support.")
endif()
