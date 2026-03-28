cmake_minimum_required(VERSION 3.20)

project(PMEMCrashEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PMEM_ENGINE_BUILD_TESTS "Build PMEM crash simulation tests" ON)

add_library(pmem_crash_engine
    src/PersistentLinkedList.cpp
)

target_include_directories(pmem_crash_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(pmem_crash_engine
    PUBLIC
        pthread
)

if(PMEM_ENGINE_BUILD_TESTS)
    enable_testing()

    add_executable(crash_recovery_test
        tests/CrashSimulation.cpp
    )

    target_link_libraries(crash_recovery_test
        PRIVATE
            pmem_crash_engine
            pthread
    )

    add_test(
        NAME crash_recovery_test
        COMMAND crash_recovery_test
    )
endif()
