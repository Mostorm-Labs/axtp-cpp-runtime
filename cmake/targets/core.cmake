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

if(NOT TARGET axtp::core)
    add_library(axtp::core ALIAS axtp_core)
endif()
if(NOT TARGET axtp::broker)
    add_library(axtp::broker ALIAS axtp_broker)
endif()
if(NOT TARGET axtp::runtime)
    add_library(axtp::runtime ALIAS axtp_runtime)
endif()
if(NOT TARGET axtp::transport_tcp_native)
    add_library(axtp::transport_tcp_native ALIAS axtp_transport_tcp_native)
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

    set(AXTP_THIRDPARTY_IXWEBSOCKET_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/IXWebSocket)
    if(EXISTS "${AXTP_THIRDPARTY_IXWEBSOCKET_DIR}/CMakeLists.txt" AND NOT TARGET ixwebsocket::ixwebsocket)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
        set(USE_TLS OFF CACHE BOOL "" FORCE)
        set(USE_ZLIB OFF CACHE BOOL "" FORCE)
        set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            ${AXTP_THIRDPARTY_IXWEBSOCKET_DIR}
            ${AXTP_CPP_RUNTIME_BINARY_DIR}/third_party/IXWebSocket
            EXCLUDE_FROM_ALL
        )
    endif()
    if(NOT TARGET ixwebsocket::ixwebsocket)
        find_package(ixwebsocket CONFIG REQUIRED)
    endif()

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

    set(AXTP_THIRDPARTY_WEBSOCKETPP_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/websocketpp)
    set(AXTP_THIRDPARTY_ASIO_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/asio)
    set(AXTP_THIRDPARTY_ASIO_INCLUDE_DIR "")
    if(EXISTS "${AXTP_THIRDPARTY_ASIO_DIR}/include/asio/version.hpp")
        set(AXTP_THIRDPARTY_ASIO_INCLUDE_DIR ${AXTP_THIRDPARTY_ASIO_DIR}/include)
    elseif(EXISTS "${AXTP_THIRDPARTY_ASIO_DIR}/asio/include/asio/version.hpp")
        set(AXTP_THIRDPARTY_ASIO_INCLUDE_DIR ${AXTP_THIRDPARTY_ASIO_DIR}/asio/include)
    endif()
    if(EXISTS "${AXTP_THIRDPARTY_WEBSOCKETPP_DIR}/websocketpp" AND
       AXTP_THIRDPARTY_ASIO_INCLUDE_DIR)
        add_library(axtp_transport_websocket_websocketpp INTERFACE)
        target_include_directories(axtp_transport_websocket_websocketpp
            INTERFACE
                $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
                $<BUILD_INTERFACE:${AXTP_THIRDPARTY_WEBSOCKETPP_DIR}>
                $<BUILD_INTERFACE:${AXTP_THIRDPARTY_ASIO_INCLUDE_DIR}>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        )
        target_compile_definitions(axtp_transport_websocket_websocketpp
            INTERFACE
                ASIO_STANDALONE
                _WEBSOCKETPP_CPP11_STL_
        )
        target_link_libraries(axtp_transport_websocket_websocketpp
            INTERFACE
                axtp_runtime
        )
        target_compile_features(axtp_transport_websocket_websocketpp INTERFACE cxx_std_17)
        set_target_properties(axtp_transport_websocket_websocketpp PROPERTIES EXPORT_NAME transport_websocket_websocketpp)
        if(NOT TARGET axtp::transport_websocket_websocketpp)
            add_library(axtp::transport_websocket_websocketpp ALIAS axtp_transport_websocket_websocketpp)
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

    set(AXTP_HIDAPI_TARGET "")
    set(AXTP_THIRDPARTY_HIDAPI_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/hidapi)
    if(EXISTS "${AXTP_THIRDPARTY_HIDAPI_DIR}/CMakeLists.txt" AND NOT TARGET hidapi::hidapi)
        set(HIDAPI_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
        set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
        set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            ${AXTP_THIRDPARTY_HIDAPI_DIR}
            ${AXTP_CPP_RUNTIME_BINARY_DIR}/third_party/hidapi
            EXCLUDE_FROM_ALL
        )
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
