if(NOT TARGET axtp_sdk OR
   NOT TARGET axtp_json_rpc OR
   NOT TARGET axtp_transport_hidapi OR
   NOT TARGET axtp_transport_tcp_native OR
   NOT TARGET axtp_transport_websocket_ix)
    message(FATAL_ERROR "axtp_toolkit requires SDK, JSON-RPC, HID, TCP, and WebSocket targets")
endif()

add_library(axtp_toolkit STATIC
    ${AXTP_CPP_RUNTIME_ROOT}/tools/toolkit/src/axtp_toolkit.cpp
)

target_include_directories(axtp_toolkit
    PUBLIC
        ${AXTP_CPP_RUNTIME_ROOT}/tools/toolkit/include
)

target_link_libraries(axtp_toolkit
    PUBLIC
        axtp_sdk
        axtp_json_rpc
        axtp_transport_hidapi
        axtp_transport_tcp_native
        axtp_transport_websocket_ix
)

target_compile_features(axtp_toolkit PUBLIC cxx_std_17)
