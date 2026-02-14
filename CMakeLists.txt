cmake_minimum_required(VERSION 3.20)
project(ApexLOB VERSION 1.0.0 LANGUAGES CXX)

# C++20 standard required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags for ultra-low-latency
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Disable exceptions and RTTI for latency-sensitive code
# Note: Comment out if you need exceptions
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti")

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files
set(SOURCES
    src/Order.cpp
    src/OrderBook.cpp
    src/ObjectPool.cpp
    src/SPSCRingBuffer.cpp
    src/MatchingEngine.cpp
    src/WireProtocol.cpp
    src/main.cpp
)

# Main executable
add_executable(apexlob ${SOURCES})

# Link libraries
target_link_libraries(apexlob pthread)

# ============================================================================
# Google Test Setup
# ============================================================================

option(BUILD_TESTING "Build tests" ON)

if(BUILD_TESTING)
    enable_testing()
    
    # Download and setup Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    
    # Test source files
    set(TEST_SOURCES
        tests/test_order_book.cpp
        tests/test_object_pool.cpp
        tests/test_ring_buffer.cpp
    )
    
    # Create test executable
    add_executable(apexlob_tests ${TEST_SOURCES})
    
    # Link Google Test
    target_link_libraries(apexlob_tests
        GTest::gtest_main
        GTest::gtest
        pthread
    )
    
    # Include test directory
    target_include_directories(apexlob_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    
    # Register tests
    include(GoogleTest)
    gtest_discover_tests(apexlob_tests)
endif()

# ============================================================================
# Installation (optional)
# ============================================================================

install(TARGETS apexlob
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)
