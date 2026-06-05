cmake_minimum_required(VERSION 3.16)

project(axtp_sdk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET axtp_core)
    set(AXTP_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()

find_package(Boost CONFIG REQUIRED COMPONENTS json)

set(AXTP_SDK_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(AXTP_SDK_IS_TOP_LEVEL ON)
endif()
option(AXTP_SDK_BUILD_TESTS "Build AXTP C++ SDK tests" ${AXTP_SDK_IS_TOP_LEVEL})

add_library(axtp_sdk INTERFACE)

target_include_directories(axtp_sdk
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(axtp_sdk
    INTERFACE
        axtp_core
        Boost::json
)
target_compile_features(axtp_sdk INTERFACE cxx_std_17)

if(NOT AXTP_SDK_BUILD_TESTS)
    return()
endif()

enable_testing()

add_executable(cpp_sdk_smoke_test
    tests/sdk_smoke_test.cpp
)

target_link_libraries(cpp_sdk_smoke_test PRIVATE axtp_sdk)

add_test(NAME cpp_sdk_smoke_test COMMAND cpp_sdk_smoke_test)
