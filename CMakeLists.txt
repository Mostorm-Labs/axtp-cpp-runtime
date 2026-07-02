cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AXTP_CPP_RUNTIME_VERSION LIMIT_COUNT 1)
project(axtp_cpp_runtime VERSION ${AXTP_CPP_RUNTIME_VERSION} LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(AXTP_CPP_RUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(AXTP_CPP_RUNTIME_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

set(AXTP_CPP_RUNTIME_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(AXTP_CPP_RUNTIME_IS_TOP_LEVEL ON)
endif()

option(AXTP_CPP_RUNTIME_BUILD_SDK "Build the AXTP SDK target" ON)
option(AXTP_CPP_RUNTIME_BUILD_TOOLS "Build AXTP command line tools" OFF)
option(AXTP_CPP_RUNTIME_BUILD_MEDIAHOST "Build the AXTP MediaHost sample" OFF)
option(AXTP_CPP_RUNTIME_TOOLS_FETCH_DEPS
    "Allow tool builds to use bundled or fetched third-party transport dependencies"
    OFF)
option(AXTP_CPP_RUNTIME_BUILD_TESTS "Build AXTP C++ runtime tests" ${AXTP_CPP_RUNTIME_IS_TOP_LEVEL})
option(AXTP_CPP_RUNTIME_BUILD_CONFORMANCE "Build AXTP conformance runner" OFF)
option(AXTP_CPP_RUNTIME_ENABLE_INSTALL "Install AXTP C++ runtime package targets" ${AXTP_CPP_RUNTIME_IS_TOP_LEVEL})
option(AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS "Install optional transport targets that were built" OFF)
option(AXTP_BUILD_JSON_RPC "Build optional AXTP WebSocket JSON-RPC helpers" OFF)
option(AXTP_BUILD_OPTIONAL_TRANSPORTS "Build optional AXTP transport targets" OFF)

function(axtp_cpp_runtime_target_is_imported target output_variable)
    if(TARGET ${target})
        get_target_property(AXTP_CPP_RUNTIME_TARGET_IMPORTED ${target} IMPORTED)
        if(AXTP_CPP_RUNTIME_TARGET_IMPORTED)
            set(${output_variable} ON PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${output_variable} OFF PARENT_SCOPE)
endfunction()

if(AXTP_CPP_RUNTIME_BUILD_TOOLS OR
   AXTP_CPP_RUNTIME_BUILD_MEDIAHOST)
    set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
    set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
endif()

if(AXTP_CPP_RUNTIME_BUILD_CONFORMANCE)
    set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
endif()

if(AXTP_CPP_RUNTIME_BUILD_TESTS)
    enable_testing()
endif()

include(cmake/targets/tool_deps.cmake)
include(cmake/targets/core.cmake)
include(cmake/targets/stream.cmake)
include(cmake/targets/media_profile.cmake)

set(AXTP_CPP_RUNTIME_NEEDS_SDK ${AXTP_CPP_RUNTIME_BUILD_SDK})
if(AXTP_CPP_RUNTIME_BUILD_TESTS OR
   AXTP_CPP_RUNTIME_BUILD_TOOLS OR
   AXTP_CPP_RUNTIME_BUILD_MEDIAHOST)
    set(AXTP_CPP_RUNTIME_NEEDS_SDK ON)
endif()

if(NOT AXTP_CPP_RUNTIME_BUILD_SDK AND AXTP_CPP_RUNTIME_NEEDS_SDK)
    message(STATUS "Enabling axtp_sdk target because tests/tools/mediahost require it")
endif()

if(AXTP_CPP_RUNTIME_NEEDS_SDK)
    include(cmake/targets/sdk.cmake)
endif()

if(AXTP_CPP_RUNTIME_BUILD_TOOLS OR
   AXTP_CPP_RUNTIME_BUILD_MEDIAHOST)
    include(cmake/targets/toolkit.cmake)
endif()

if(AXTP_CPP_RUNTIME_BUILD_TOOLS)
    include(cmake/targets/axtpctl.cmake)
endif()

if(AXTP_CPP_RUNTIME_BUILD_MEDIAHOST)
    include(cmake/targets/mediahost.cmake)
endif()

if(AXTP_CPP_RUNTIME_BUILD_CONFORMANCE)
    if(NOT TARGET axtp_json_rpc)
        message(FATAL_ERROR "AXTP conformance runner requires AXTP_BUILD_JSON_RPC")
    endif()

    add_executable(axtp_conformance_runner
        devtools/conformance/conformance_runner.cpp
    )
    target_link_libraries(axtp_conformance_runner PRIVATE axtp_core axtp_json_rpc)
endif()

if(AXTP_CPP_RUNTIME_BUILD_TESTS)
    include(tests/cmake/verify_target_boundaries.cmake)
    include(cmake/targets/tests.cmake)
endif()

if(AXTP_CPP_RUNTIME_ENABLE_INSTALL)
    set(AXTP_CPP_RUNTIME_INSTALL_TARGETS
        axtp_core
        axtp_broker
        axtp_runtime
        axtp_stream
        axtp_media_profile
    )
    if(TARGET axtp_sdk)
        list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_sdk)
    endif()
    if(TARGET axtp_json_rpc)
        list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_json_rpc)
    endif()

    set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_BOOST OFF)
    set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_IXWEBSOCKET OFF)
    set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_HIDAPI OFF)

    if(AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS)
        if(TARGET axtp_transport_tcp_native)
            list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_transport_tcp_native)
        endif()
        if(TARGET axtp_transport_tcp_boost)
            list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_transport_tcp_boost)
            set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_BOOST ON)
        endif()
        if(TARGET axtp_transport_websocket_boost)
            list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_transport_websocket_boost)
            set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_BOOST ON)
        endif()
        if(TARGET axtp_transport_websocket_ix)
            axtp_cpp_runtime_target_is_imported(ixwebsocket AXTP_IXWEBSOCKET_IS_IMPORTED)
            axtp_cpp_runtime_target_is_imported(ixwebsocket::ixwebsocket AXTP_IXWEBSOCKET_NAMESPACED_IS_IMPORTED)
            if(AXTP_IXWEBSOCKET_IS_IMPORTED OR AXTP_IXWEBSOCKET_NAMESPACED_IS_IMPORTED)
                list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_transport_websocket_ix)
                set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_IXWEBSOCKET ON)
            else()
                message(STATUS "Skipping install export for axtp_transport_websocket_ix because IXWebSocket is built from the vendored submodule")
            endif()
        endif()
        if(TARGET axtp_transport_websocket_websocketpp)
            list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_transport_websocket_websocketpp)
        endif()
        if(TARGET axtp_transport_hidapi)
            set(AXTP_HIDAPI_VENDOR_TARGET_PRESENT OFF)
            foreach(AXTP_HIDAPI_VENDOR_TARGET hidapi_darwin hidapi_hidraw hidapi_libusb)
                if(TARGET ${AXTP_HIDAPI_VENDOR_TARGET})
                    axtp_cpp_runtime_target_is_imported(${AXTP_HIDAPI_VENDOR_TARGET} AXTP_HIDAPI_VENDOR_TARGET_IMPORTED)
                    if(NOT AXTP_HIDAPI_VENDOR_TARGET_IMPORTED)
                        set(AXTP_HIDAPI_VENDOR_TARGET_PRESENT ON)
                    endif()
                endif()
            endforeach()
            if(AXTP_HIDAPI_VENDOR_TARGET_PRESENT)
                message(STATUS "Skipping install export for axtp_transport_hidapi because hidapi is built from the vendored submodule")
            else()
                list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_transport_hidapi)
                set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_HIDAPI ON)
            endif()
        endif()
    endif()

    install(TARGETS ${AXTP_CPP_RUNTIME_INSTALL_TARGETS}
        EXPORT axtp_cpp_runtimeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY
            include/core
            include/sdk
            include/stream
            include/profiles
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES
            include/axtp_core.hpp
            include/axtp_runtime.hpp
            include/axtp_sdk.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    if(TARGET axtp_json_rpc)
        install(DIRECTORY include/json_rpc
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        install(FILES include/axtp_json_rpc.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()
    if(AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS)
        install(DIRECTORY include/transports
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        install(FILES
                include/axtp_transport_hid.hpp
                include/axtp_transport_tcp_native.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()

    set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON ON)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include/nlohmann/json.hpp")
        set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON OFF)
        install(DIRECTORY third_party/json/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()

    set(AXTP_CPP_RUNTIME_CONFIG_FIND_NLOHMANN_JSON "")
    if(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON)
        set(AXTP_CPP_RUNTIME_CONFIG_FIND_NLOHMANN_JSON
            "find_dependency(nlohmann_json CONFIG REQUIRED)")
    endif()

    set(AXTP_CPP_RUNTIME_CONFIG_FIND_BOOST "")
    if(AXTP_CPP_RUNTIME_CONFIG_NEEDS_BOOST)
        set(AXTP_CPP_RUNTIME_CONFIG_FIND_BOOST
            "find_dependency(Boost CONFIG REQUIRED)")
    endif()

    set(AXTP_CPP_RUNTIME_CONFIG_FIND_IXWEBSOCKET "")
    if(AXTP_CPP_RUNTIME_CONFIG_NEEDS_IXWEBSOCKET)
        set(AXTP_CPP_RUNTIME_CONFIG_FIND_IXWEBSOCKET
            "find_dependency(ixwebsocket CONFIG REQUIRED)")
    endif()

    set(AXTP_CPP_RUNTIME_CONFIG_FIND_HIDAPI "")
    if(AXTP_CPP_RUNTIME_CONFIG_NEEDS_HIDAPI)
        set(AXTP_CPP_RUNTIME_CONFIG_FIND_HIDAPI [=[
find_dependency(hidapi CONFIG QUIET)
if(NOT TARGET hidapi::hidapi AND
   NOT TARGET hidapi::hidapi-shared AND
   NOT TARGET hidapi::hidapi-static AND
   NOT TARGET hidapi::hidapi-darwin AND
   NOT TARGET hidapi::hidapi-hidraw AND
   NOT TARGET hidapi::hidapi-libusb AND
   NOT TARGET hidapi::darwin AND
   NOT TARGET hidapi::hidraw AND
   NOT TARGET hidapi::libusb)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi)
        if(NOT TARGET PkgConfig::HIDAPI)
            pkg_check_modules(HIDAPI_HIDRAW QUIET IMPORTED_TARGET hidapi-hidraw)
        endif()
        if(NOT TARGET PkgConfig::HIDAPI AND
           NOT TARGET PkgConfig::HIDAPI_HIDRAW)
            pkg_check_modules(HIDAPI_LIBUSB QUIET IMPORTED_TARGET hidapi-libusb)
        endif()
    endif()
endif()
]=])
    endif()

    if(AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS AND
       TARGET axtp_transport_websocket_websocketpp)
        install(DIRECTORY third_party/websocketpp/websocketpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        install(DIRECTORY third_party/asio/asio/include/asio
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        install(FILES third_party/asio/asio/include/asio.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()

    set(AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR
        ${CMAKE_INSTALL_LIBDIR}/cmake/axtp_cpp_runtime
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/axtp_cpp_runtimeConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfig.cmake
        INSTALL_DESTINATION ${AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR}
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(EXPORT axtp_cpp_runtimeTargets
        NAMESPACE axtp::
        DESTINATION ${AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR}
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfigVersion.cmake
        DESTINATION ${AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR}
    )
endif()
