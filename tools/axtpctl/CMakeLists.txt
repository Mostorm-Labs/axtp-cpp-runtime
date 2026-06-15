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

add_executable(axtpctl
    src/hid_main.cpp
)

target_link_libraries(axtpctl
    PRIVATE
        axtp_sdk
        axtp_transport_hidapi
)

option(AXTPCTL_BUILD_FULL "Build legacy all-transport axtpctl target" OFF)
if(AXTPCTL_BUILD_FULL)
    add_executable(axtpctl_full EXCLUDE_FROM_ALL
        src/main.cpp
    )

    target_link_libraries(axtpctl_full
        PRIVATE
            axtp_sdk
            axtp_json_rpc
            axtp_transport_hidapi
            axtp_transport_tcp_boost
            axtp_transport_websocket_ix
    )
endif()

add_executable(axtp_hid_audio_device_demo
    src/hid_audio_device_demo.cpp
)

target_link_libraries(axtp_hid_audio_device_demo
    PRIVATE
        axtp_sdk
        axtp_transport_hidapi
)

enable_testing()

add_test(NAME axtpctl_help COMMAND axtpctl --help)
add_test(NAME axtpctl_capability_methods COMMAND axtpctl capability methods)
add_test(NAME axtpctl_list_methods COMMAND axtpctl list-methods)

add_executable(hid_local_backend_test
    tests/hid_local_backend_test.cpp
)
target_link_libraries(hid_local_backend_test PRIVATE axtp_transport_hidapi)
add_test(NAME hid_local_backend_test COMMAND hid_local_backend_test)

add_executable(hid_audio_demo_e2e_test
    tests/hid_audio_demo_e2e_test.cpp
)
target_include_directories(hid_audio_demo_e2e_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(hid_audio_demo_e2e_test
    PRIVATE
        axtp_sdk
        axtp_transport_hidapi
)
add_test(NAME hid_audio_demo_e2e_test COMMAND hid_audio_demo_e2e_test)

add_test(NAME axtpctl_unknown_method
    COMMAND axtpctl -c unknown.method
)
set_tests_properties(axtpctl_unknown_method PROPERTIES WILL_FAIL TRUE)

add_test(NAME axtpctl_json_conflict
    COMMAND axtpctl -c audio.setAlgorithmConfig --json {} --json-file ${CMAKE_CURRENT_SOURCE_DIR}/tests/params_algorithm_config.json
)
set_tests_properties(axtpctl_json_conflict PROPERTIES WILL_FAIL TRUE)
