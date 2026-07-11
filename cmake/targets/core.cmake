add_library(axtp_core INTERFACE)
target_include_directories(axtp_core
    INTERFACE
        $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(AXTP_THIRDPARTY_JSON_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/json)
set(AXTP_BUNDLED_JSON_AVAILABLE OFF)
if(EXISTS "${AXTP_THIRDPARTY_JSON_DIR}/CMakeLists.txt" AND NOT TARGET nlohmann_json::nlohmann_json)
    set(AXTP_BUNDLED_JSON_AVAILABLE ON)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    add_subdirectory(
        ${AXTP_THIRDPARTY_JSON_DIR}
        ${AXTP_CPP_RUNTIME_BINARY_DIR}/third_party/json
        EXCLUDE_FROM_ALL
    )
elseif(EXISTS "${AXTP_THIRDPARTY_JSON_DIR}/include/nlohmann/json.hpp")
    set(AXTP_BUNDLED_JSON_AVAILABLE ON)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

set(AXTP_JSON_INSTALL_LINK "")
if(NOT AXTP_BUNDLED_JSON_AVAILABLE)
    set(AXTP_JSON_INSTALL_LINK "$<INSTALL_INTERFACE:nlohmann_json::nlohmann_json>")
endif()

target_link_libraries(axtp_core
    INTERFACE
        $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
        ${AXTP_JSON_INSTALL_LINK}
)
target_compile_features(axtp_core INTERFACE cxx_std_17)
set_target_properties(axtp_core PROPERTIES EXPORT_NAME core)

add_library(axtp_broker INTERFACE)
target_link_libraries(axtp_broker INTERFACE axtp_core)
target_compile_features(axtp_broker INTERFACE cxx_std_17)
set_target_properties(axtp_broker PROPERTIES EXPORT_NAME broker)

add_library(axtp_runtime INTERFACE)
target_link_libraries(axtp_runtime INTERFACE axtp_core axtp_broker)
target_compile_features(axtp_runtime INTERFACE cxx_std_17)
set_target_properties(axtp_runtime PROPERTIES EXPORT_NAME runtime)

if(NOT TARGET axtp::core)
    add_library(axtp::core ALIAS axtp_core)
endif()
if(NOT TARGET axtp::broker)
    add_library(axtp::broker ALIAS axtp_broker)
endif()
if(NOT TARGET axtp::runtime)
    add_library(axtp::runtime ALIAS axtp_runtime)
endif()
if(AXTP_BUILD_JSON_RPC)
    add_library(axtp_json_rpc INTERFACE)
    target_include_directories(axtp_json_rpc
        INTERFACE
            $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(axtp_json_rpc
        INTERFACE
            axtp_core
    )
    target_compile_features(axtp_json_rpc INTERFACE cxx_std_17)
    set_target_properties(axtp_json_rpc PROPERTIES EXPORT_NAME json_rpc)
    if(NOT TARGET axtp::json_rpc)
        add_library(axtp::json_rpc ALIAS axtp_json_rpc)
    endif()
endif()

if(AXTP_BUILD_OPTIONAL_TRANSPORTS)
    add_library(axtp_transport_tcp_native INTERFACE)
    target_include_directories(axtp_transport_tcp_native
        INTERFACE
            $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(axtp_transport_tcp_native
        INTERFACE
            axtp_runtime
    )
    if(WIN32)
        target_link_libraries(axtp_transport_tcp_native INTERFACE ws2_32)
    endif()
    target_compile_features(axtp_transport_tcp_native INTERFACE cxx_std_17)
    set_target_properties(axtp_transport_tcp_native PROPERTIES EXPORT_NAME transport_tcp_native)
    if(NOT TARGET axtp::transport_tcp_native)
        add_library(axtp::transport_tcp_native ALIAS axtp_transport_tcp_native)
    endif()

    find_package(Boost CONFIG QUIET)

    if(Boost_FOUND)
        add_library(axtp_transport_tcp_boost INTERFACE)
        target_include_directories(axtp_transport_tcp_boost
            INTERFACE
                $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_link_libraries(axtp_transport_tcp_boost
            INTERFACE
                axtp_runtime
                Boost::headers
        )
        target_compile_features(axtp_transport_tcp_boost INTERFACE cxx_std_17)
        set_target_properties(axtp_transport_tcp_boost PROPERTIES EXPORT_NAME transport_tcp_boost)
        if(NOT TARGET axtp::transport_tcp_boost)
            add_library(axtp::transport_tcp_boost ALIAS axtp_transport_tcp_boost)
        endif()
    endif()

    if(NOT TARGET ixwebsocket::ixwebsocket)
        axtp_cpp_runtime_resolve_ixwebsocket()
    endif()

    if(TARGET ixwebsocket::ixwebsocket)
        add_library(axtp_transport_websocket_ix INTERFACE)
        target_include_directories(axtp_transport_websocket_ix
            INTERFACE
                $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_link_libraries(axtp_transport_websocket_ix
            INTERFACE
                axtp_runtime
                ixwebsocket::ixwebsocket
        )
        target_compile_features(axtp_transport_websocket_ix INTERFACE cxx_std_17)
        set_target_properties(axtp_transport_websocket_ix PROPERTIES EXPORT_NAME transport_websocket_ix)
        if(NOT TARGET axtp::transport_websocket_ix)
            add_library(axtp::transport_websocket_ix ALIAS axtp_transport_websocket_ix)
        endif()
    endif()

    if(Boost_FOUND)
        add_library(axtp_transport_websocket_boost INTERFACE)
        target_include_directories(axtp_transport_websocket_boost
            INTERFACE
                $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_link_libraries(axtp_transport_websocket_boost
            INTERFACE
                axtp_runtime
                Boost::headers
        )
        target_compile_features(axtp_transport_websocket_boost INTERFACE cxx_std_17)
        set_target_properties(axtp_transport_websocket_boost PROPERTIES EXPORT_NAME transport_websocket_boost)
        if(NOT TARGET axtp::transport_websocket_boost)
            add_library(axtp::transport_websocket_boost ALIAS axtp_transport_websocket_boost)
        endif()
    endif()

    if(AXTP_HIDAPI_TARGET AND NOT TARGET ${AXTP_HIDAPI_TARGET})
        set(AXTP_HIDAPI_TARGET "")
    endif()

    if(NOT AXTP_HIDAPI_TARGET)
        axtp_cpp_runtime_resolve_hidapi()
    endif()

    if(AXTP_HIDAPI_TARGET)
        add_library(axtp_transport_hidapi STATIC EXCLUDE_FROM_ALL
            ${AXTP_CPP_RUNTIME_ROOT}/src/transports/hidapi/hid_transport.cpp
        )
        target_include_directories(axtp_transport_hidapi
            PUBLIC
                $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_link_libraries(axtp_transport_hidapi
            PUBLIC
                axtp_runtime
            PRIVATE
                ${AXTP_HIDAPI_TARGET}
        )
        if(WIN32)
            target_link_libraries(axtp_transport_hidapi PRIVATE hid)
        endif()
        target_compile_features(axtp_transport_hidapi PUBLIC cxx_std_17)
        set_target_properties(axtp_transport_hidapi PROPERTIES EXPORT_NAME transport_hidapi)
        if(NOT TARGET axtp::transport_hidapi)
            add_library(axtp::transport_hidapi ALIAS axtp_transport_hidapi)
        endif()
    endif()
endif()
