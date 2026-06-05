cmake_minimum_required(VERSION 3.16)

project(axtp_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(axtp_core INTERFACE)
target_include_directories(axtp_core
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
find_package(Boost CONFIG REQUIRED COMPONENTS json)

target_link_libraries(axtp_core
    INTERFACE
        Boost::headers
        Boost::json
)
target_compile_features(axtp_core INTERFACE cxx_std_17)

add_library(axtp_broker INTERFACE)
target_link_libraries(axtp_broker INTERFACE axtp_core)
target_compile_features(axtp_broker INTERFACE cxx_std_17)

add_library(axtp_runtime INTERFACE)
target_link_libraries(axtp_runtime INTERFACE axtp_core axtp_broker)
target_compile_features(axtp_runtime INTERFACE cxx_std_17)

set(AXTP_CORE_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(AXTP_CORE_IS_TOP_LEVEL ON)
endif()

option(AXTP_BUILD_JSON_RPC "Build optional AXTP WebSocket JSON-RPC headers/tests" ${AXTP_CORE_IS_TOP_LEVEL})
option(AXTP_BUILD_OPTIONAL_TRANSPORTS "Build optional AXTP example transport targets" ${AXTP_CORE_IS_TOP_LEVEL})
option(AXTP_CORE_BUILD_TESTS "Build AXTP core runtime tests" ${AXTP_CORE_IS_TOP_LEVEL})

if(AXTP_BUILD_JSON_RPC)
    add_library(axtp_json_rpc INTERFACE)
    target_include_directories(axtp_json_rpc
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/../json-rpc/include
    )
    target_link_libraries(axtp_json_rpc
        INTERFACE
            axtp_core
    )
    target_compile_features(axtp_json_rpc INTERFACE cxx_std_17)
endif()

if(AXTP_BUILD_OPTIONAL_TRANSPORTS)
    add_library(axtp_transport_tcp_boost INTERFACE)
    target_include_directories(axtp_transport_tcp_boost
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/../transports/include
    )
    target_link_libraries(axtp_transport_tcp_boost
        INTERFACE
            axtp_runtime
            Boost::headers
    )
    target_compile_features(axtp_transport_tcp_boost INTERFACE cxx_std_17)

    add_library(axtp_transport_websocket_boost INTERFACE)
    target_include_directories(axtp_transport_websocket_boost
        INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/../transports/include
    )
    target_link_libraries(axtp_transport_websocket_boost
        INTERFACE
            axtp_runtime
            Boost::headers
    )
    target_compile_features(axtp_transport_websocket_boost INTERFACE cxx_std_17)

    set(AXTP_HIDAPI_TARGET "")
    set(AXTP_THIRDPARTY_HIDAPI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/hidapi)
    if(EXISTS ${AXTP_THIRDPARTY_HIDAPI_DIR}/CMakeLists.txt AND NOT TARGET hidapi::hidapi)
        set(HIDAPI_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
        set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
        set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        add_subdirectory(${AXTP_THIRDPARTY_HIDAPI_DIR} ${CMAKE_CURRENT_BINARY_DIR}/thirdparty/hidapi EXCLUDE_FROM_ALL)
    endif()

    if(TARGET hidapi::hidapi)
        set(AXTP_HIDAPI_TARGET hidapi::hidapi)
    endif()

    if(NOT AXTP_HIDAPI_TARGET)
        find_package(hidapi CONFIG QUIET)
        if(TARGET hidapi::hidapi)
            set(AXTP_HIDAPI_TARGET hidapi::hidapi)
        elseif(TARGET hidapi::hidapi-shared)
            set(AXTP_HIDAPI_TARGET hidapi::hidapi-shared)
        elseif(TARGET hidapi::hidapi-static)
            set(AXTP_HIDAPI_TARGET hidapi::hidapi-static)
        elseif(TARGET hidapi::hidapi-darwin)
            set(AXTP_HIDAPI_TARGET hidapi::hidapi-darwin)
        elseif(TARGET hidapi::hidapi-hidraw)
            set(AXTP_HIDAPI_TARGET hidapi::hidapi-hidraw)
        elseif(TARGET hidapi::hidapi-libusb)
            set(AXTP_HIDAPI_TARGET hidapi::hidapi-libusb)
        elseif(TARGET hidapi::darwin)
            set(AXTP_HIDAPI_TARGET hidapi::darwin)
        elseif(TARGET hidapi::hidraw)
            set(AXTP_HIDAPI_TARGET hidapi::hidraw)
        elseif(TARGET hidapi::libusb)
            set(AXTP_HIDAPI_TARGET hidapi::libusb)
        endif()
    endif()

    if(NOT AXTP_HIDAPI_TARGET)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi)
            if(TARGET PkgConfig::HIDAPI)
                set(AXTP_HIDAPI_TARGET PkgConfig::HIDAPI)
            else()
                pkg_check_modules(HIDAPI_HIDRAW QUIET IMPORTED_TARGET hidapi-hidraw)
                if(TARGET PkgConfig::HIDAPI_HIDRAW)
                    set(AXTP_HIDAPI_TARGET PkgConfig::HIDAPI_HIDRAW)
                else()
                    pkg_check_modules(HIDAPI_LIBUSB QUIET IMPORTED_TARGET hidapi-libusb)
                    if(TARGET PkgConfig::HIDAPI_LIBUSB)
                        set(AXTP_HIDAPI_TARGET PkgConfig::HIDAPI_LIBUSB)
                    endif()
                endif()
            endif()
        endif()
    endif()

    if(NOT AXTP_HIDAPI_TARGET)
        message(FATAL_ERROR "hidapi is required to build optional axtp_transport_hidapi")
    endif()

    add_library(axtp_transport_hidapi STATIC EXCLUDE_FROM_ALL
        ${CMAKE_CURRENT_SOURCE_DIR}/../transports/src/hidapi/hid_transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../transports/src/hidapi/hid_local_backend.cpp
    )
    target_include_directories(axtp_transport_hidapi
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/../transports/include
    )
    target_link_libraries(axtp_transport_hidapi
        PUBLIC
            axtp_runtime
        PRIVATE
            ${AXTP_HIDAPI_TARGET}
    )
    target_compile_features(axtp_transport_hidapi PUBLIC cxx_std_17)
endif()

if(NOT AXTP_CORE_BUILD_TESTS)
    return()
endif()

if(NOT TARGET axtp_json_rpc OR
   NOT TARGET axtp_transport_tcp_boost OR
   NOT TARGET axtp_transport_websocket_boost OR
   NOT TARGET axtp_transport_hidapi)
    message(FATAL_ERROR "AXTP core tests require AXTP_BUILD_JSON_RPC and AXTP_BUILD_OPTIONAL_TRANSPORTS")
endif()

enable_testing()

add_executable(phase1_model_io_test
    tests/phase1_model_io_test.cpp
)
target_link_libraries(phase1_model_io_test PRIVATE axtp_core)
add_test(NAME phase1_model_io_test COMMAND phase1_model_io_test)

add_executable(phase2_inbound_test
    tests/phase2_inbound_test.cpp
)
target_link_libraries(phase2_inbound_test PRIVATE axtp_core)
add_test(NAME phase2_inbound_test COMMAND phase2_inbound_test)

add_executable(phase3_outbound_test
    tests/phase3_outbound_test.cpp
)
target_link_libraries(phase3_outbound_test PRIVATE axtp_core)
add_test(NAME phase3_outbound_test COMMAND phase3_outbound_test)

add_executable(phase4_core_test
    tests/phase4_core_test.cpp
)
target_link_libraries(phase4_core_test PRIVATE axtp_core)
add_test(NAME phase4_core_test COMMAND phase4_core_test)

add_executable(phase5_transport_test
    tests/phase5_transport_test.cpp
)
target_link_libraries(phase5_transport_test PRIVATE axtp_core)
add_test(NAME phase5_transport_test COMMAND phase5_transport_test)

add_executable(phase6_real_transport_test
    tests/phase6_real_transport_test.cpp
)
target_link_libraries(phase6_real_transport_test
    PRIVATE
        axtp_core
        axtp_json_rpc
        axtp_transport_tcp_boost
        axtp_transport_websocket_boost
        Boost::json
)
add_test(NAME phase6_real_transport_test COMMAND phase6_real_transport_test)

add_executable(phase7_broker_test
    tests/phase7_broker_test.cpp
)
target_link_libraries(phase7_broker_test PRIVATE axtp_core)
add_test(NAME phase7_broker_test COMMAND phase7_broker_test)

add_executable(phase8_api_surface_test
    tests/phase8_api_surface_test.cpp
)
target_link_libraries(phase8_api_surface_test PRIVATE axtp_core)
add_test(NAME phase8_api_surface_test COMMAND phase8_api_surface_test)

add_executable(phase9_hid_transport_test
    tests/phase9_hid_transport_test.cpp
)
target_link_libraries(phase9_hid_transport_test PRIVATE axtp_transport_hidapi)
add_test(NAME phase9_hid_transport_test COMMAND phase9_hid_transport_test)
