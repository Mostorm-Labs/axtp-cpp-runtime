function(axtp_assert_interface_does_not_link target forbidden_regex)
    if(NOT TARGET ${target})
        return()
    endif()

    get_target_property(interface_links ${target} INTERFACE_LINK_LIBRARIES)
    if(NOT interface_links)
        return()
    endif()

    foreach(interface_link IN LISTS interface_links)
        if(interface_link MATCHES "${forbidden_regex}")
            message(FATAL_ERROR
                "${target} must not expose ${interface_link} through INTERFACE_LINK_LIBRARIES")
        endif()
    endforeach()
endfunction()

axtp_assert_interface_does_not_link(axtp_core "^axtp_transport_")
axtp_assert_interface_does_not_link(axtp_runtime "^axtp_transport_")
axtp_assert_interface_does_not_link(axtp_sdk "^axtp_transport_")
axtp_assert_interface_does_not_link(axtp_stream "^axtp_transport_")
axtp_assert_interface_does_not_link(axtp_media_profile "^axtp_transport_")

if(NOT AXTP_BUILD_OPTIONAL_TRANSPORTS)
    foreach(optional_transport_target
            axtp_transport_tcp_native
            axtp_transport_tcp_boost
            axtp_transport_hidapi
            axtp_transport_websocket_boost
            axtp_transport_websocket_ix
            axtp_transport_websocket_websocketpp)
        if(TARGET ${optional_transport_target})
            message(FATAL_ERROR
                "${optional_transport_target} must not be defined when "
                "AXTP_BUILD_OPTIONAL_TRANSPORTS is OFF")
        endif()
    endforeach()
endif()

if(NOT AXTP_BUILD_JSON_RPC AND TARGET axtp_json_rpc)
    message(FATAL_ERROR "axtp_json_rpc must not be defined when AXTP_BUILD_JSON_RPC is OFF")
endif()

if(NOT AXTP_CPP_RUNTIME_TOOLS_FETCH_DEPS)
    foreach(tool_dependency_target
            ixwebsocket
            hidapi_darwin
            hidapi_hidraw
            hidapi_libusb)
        if(TARGET ${tool_dependency_target})
            message(FATAL_ERROR
                "${tool_dependency_target} must not be pulled from bundled tool dependencies "
                "unless AXTP_CPP_RUNTIME_TOOLS_FETCH_DEPS is ON")
        endif()
    endforeach()
endif()
