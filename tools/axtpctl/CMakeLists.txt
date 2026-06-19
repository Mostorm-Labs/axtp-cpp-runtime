cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(axtpctl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT TARGET axtp_sdk)
    set(AXTP_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
    set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../sdk ${CMAKE_CURRENT_BINARY_DIR}/sdk)
endif()

if(NOT TARGET axtp_toolkit)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../toolkit ${CMAKE_CURRENT_BINARY_DIR}/toolkit)
endif()

add_executable(axtpctl
    src/main.cpp
)

target_link_libraries(axtpctl
    PRIVATE
        axtp_toolkit
)

enable_testing()

add_test(NAME axtpctl_help COMMAND axtpctl --help)
add_test(NAME axtpctl_capability_methods COMMAND axtpctl capability methods)
add_test(NAME axtpctl_list_methods COMMAND axtpctl list-methods)

add_test(NAME axtpctl_unknown_method
    COMMAND axtpctl -c unknown.method
)
set_tests_properties(axtpctl_unknown_method PROPERTIES WILL_FAIL TRUE)

add_test(NAME axtpctl_json_conflict
    COMMAND axtpctl -c audio.setAlgorithmConfig --json {} --json-file ${CMAKE_CURRENT_SOURCE_DIR}/tests/params_algorithm_config.json
)
set_tests_properties(axtpctl_json_conflict PROPERTIES WILL_FAIL TRUE)

add_test(NAME axtpctl_invalid_vid
    COMMAND axtpctl -t hid list-hid --vid 0x10000
)
set_tests_properties(axtpctl_invalid_vid PROPERTIES WILL_FAIL TRUE)

add_test(NAME axtpctl_invalid_usage_page
    COMMAND axtpctl -t hid list-hid --usage-page 0x81xyz
)
set_tests_properties(axtpctl_invalid_usage_page PROPERTIES WILL_FAIL TRUE)
