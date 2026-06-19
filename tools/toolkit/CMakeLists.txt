cmake_minimum_required(VERSION 3.16)

project(axtp_toolkit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET axtp_sdk)
    set(AXTP_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
    set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../sdk ${CMAKE_CURRENT_BINARY_DIR}/sdk)
endif()

add_library(axtp_toolkit STATIC
    src/axtp_toolkit.cpp
)

target_include_directories(axtp_toolkit
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(axtp_toolkit
    PUBLIC
        axtp_sdk
        axtp_json_rpc
        axtp_transport_hidapi
        axtp_transport_tcp_boost
        axtp_transport_websocket_ix
)

target_compile_features(axtp_toolkit PUBLIC cxx_std_17)
