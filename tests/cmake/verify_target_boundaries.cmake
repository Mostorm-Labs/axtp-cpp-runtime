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

foreach(retired_host_target
        axtp_stream
        axtp_media_profile
        axtp_toolkit
        axtp_mediahost_render_win32
        axtp-mediahost)
    if(TARGET ${retired_host_target})
        message(FATAL_ERROR "retired Host-layer target must not be defined: ${retired_host_target}")
    endif()
endforeach()

foreach(retired_host_path
        "cmake/targets/stream.cmake"
        "cmake/targets/media_profile.cmake"
        "cmake/targets/mediahost.cmake"
        "cmake/targets/toolkit.cmake"
        "include/stream"
        "include/profiles/media"
        "tests/stream/stream_test.cpp"
        "tests/profiles/media_profile_test.cpp"
        "tools/axtp-mediahost"
        "tools/toolkit")
    if(EXISTS "${AXTP_CPP_RUNTIME_ROOT}/${retired_host_path}")
        message(FATAL_ERROR "retired Host-layer path must not exist: ${retired_host_path}")
    endif()
endforeach()

foreach(retired_media_option
        AXTP_CPP_RUNTIME_BUILD_MEDIAHOST
        AXTP_CPP_RUNTIME_TOOLS_FETCH_DEPS)
    if(DEFINED ${retired_media_option})
        message(FATAL_ERROR "retired MediaHost option must not be defined: ${retired_media_option}")
    endif()
endforeach()

if(NOT AXTP_BUILD_OPTIONAL_TRANSPORTS)
    foreach(optional_transport_target
            axtp_transport_tcp_native
            axtp_transport_tcp_boost
            axtp_transport_hidapi
            axtp_transport_websocket_boost
            axtp_transport_websocket_ix)
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

foreach(retired_dependency IXWebSocket asio hidapi websocketpp)
    if(EXISTS "${AXTP_CPP_RUNTIME_ROOT}/third_party/${retired_dependency}")
        message(FATAL_ERROR
            "cpp-runtime must not vendor third_party/${retired_dependency}; "
            "concrete transport providers belong to the embedding application or the explicit tool fetch path")
    endif()
endforeach()
