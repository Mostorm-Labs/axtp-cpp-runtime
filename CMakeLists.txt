cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AXTP_CPP_RUNTIME_VERSION LIMIT_COUNT 1)
project(axtp_cpp_runtime VERSION ${AXTP_CPP_RUNTIME_VERSION} LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(AXTP_CPP_RUNTIME_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(AXTP_CPP_RUNTIME_IS_TOP_LEVEL ON)
endif()

option(AXTP_CPP_RUNTIME_BUILD_SDK "Build the AXTP SDK target" ON)
option(AXTP_CPP_RUNTIME_BUILD_TOOLS "Build AXTP command line tools" OFF)
option(AXTP_CPP_RUNTIME_BUILD_MEDIAHOST "Build the AXTP MediaHost sample" OFF)
option(AXTP_CPP_RUNTIME_ENABLE_INSTALL "Install AXTP C++ runtime package targets" ${AXTP_CPP_RUNTIME_IS_TOP_LEVEL})
option(AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS "Install optional transport targets that were built" OFF)

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

if(AXTP_CPP_RUNTIME_BUILD_TOOLS OR AXTP_CPP_RUNTIME_BUILD_MEDIAHOST)
    set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
    set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
endif()

if(AXTP_CPP_RUNTIME_BUILD_TOOLS OR AXTP_CPP_RUNTIME_BUILD_MEDIAHOST OR
   AXTP_CORE_BUILD_TESTS OR AXTP_SDK_BUILD_TESTS)
    enable_testing()
endif()

if(AXTP_CPP_RUNTIME_BUILD_SDK)
    add_subdirectory(sdk)
else()
    add_subdirectory(core)
endif()

if(AXTP_CPP_RUNTIME_BUILD_TOOLS)
    add_subdirectory(tools/axtpctl)
endif()

if(AXTP_CPP_RUNTIME_BUILD_MEDIAHOST)
    add_subdirectory(tools/axtp-mediahost)
endif()

if(AXTP_CPP_RUNTIME_ENABLE_INSTALL)
    set(AXTP_CPP_RUNTIME_INSTALL_TARGETS
        axtp_core
        axtp_broker
        axtp_runtime
        axtp_transport_tcp_native
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

    install(DIRECTORY core/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    if(TARGET axtp_sdk)
        install(DIRECTORY sdk/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()
    if(TARGET axtp_json_rpc)
        install(DIRECTORY json-rpc/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()
    install(DIRECTORY transports/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON ON)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/json/include/nlohmann/json.hpp")
        set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON OFF)
        install(DIRECTORY thirdparty/json/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()

    if(AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS AND
       TARGET axtp_transport_websocket_websocketpp)
        install(DIRECTORY thirdparty/websocketpp/websocketpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        install(DIRECTORY thirdparty/asio/asio/include/asio
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        install(FILES thirdparty/asio/asio/include/asio.hpp
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
