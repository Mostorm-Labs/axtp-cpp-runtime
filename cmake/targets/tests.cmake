add_executable(phase1_model_io_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase1_model_io_test.cpp
)
target_link_libraries(phase1_model_io_test PRIVATE axtp_core)
add_test(NAME phase1_model_io_test COMMAND phase1_model_io_test)

add_executable(phase2_inbound_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase2_inbound_test.cpp
)
target_link_libraries(phase2_inbound_test PRIVATE axtp_core)
add_test(NAME phase2_inbound_test COMMAND phase2_inbound_test)

add_executable(phase3_outbound_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase3_outbound_test.cpp
)
target_link_libraries(phase3_outbound_test PRIVATE axtp_core)
add_test(NAME phase3_outbound_test COMMAND phase3_outbound_test)

add_executable(phase4_core_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase4_core_test.cpp
)
target_link_libraries(phase4_core_test PRIVATE axtp_core)
add_test(NAME phase4_core_test COMMAND phase4_core_test)

add_executable(phase5_transport_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase5_transport_test.cpp
)
target_link_libraries(phase5_transport_test PRIVATE axtp_core)
add_test(NAME phase5_transport_test COMMAND phase5_transport_test)

if(TARGET axtp_json_rpc AND
   TARGET axtp_transport_tcp_native AND
   TARGET axtp_transport_websocket_ix)
    add_executable(phase6_real_transport_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase6_real_transport_test.cpp
    )
    target_link_libraries(phase6_real_transport_test
        PRIVATE
            axtp_core
            axtp_json_rpc
            axtp_transport_tcp_native
            axtp_transport_websocket_ix
    )
    add_test(NAME phase6_real_transport_test COMMAND phase6_real_transport_test)
endif()

add_executable(phase7_broker_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase7_broker_test.cpp
)
target_link_libraries(phase7_broker_test PRIVATE axtp_core)
add_test(NAME phase7_broker_test COMMAND phase7_broker_test)

add_executable(phase8_api_surface_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase8_api_surface_test.cpp
)
target_link_libraries(phase8_api_surface_test PRIVATE axtp_core)
add_test(NAME phase8_api_surface_test COMMAND phase8_api_surface_test)

if(TARGET axtp_transport_hidapi)
    add_executable(phase9_hid_transport_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase9_hid_transport_test.cpp
    )
    target_link_libraries(phase9_hid_transport_test PRIVATE axtp_transport_hidapi)
    add_test(NAME phase9_hid_transport_test COMMAND phase9_hid_transport_test)
endif()

if(TARGET axtp_sdk)
    add_executable(cpp_sdk_smoke_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/sdk/sdk_smoke_test.cpp
    )
    target_link_libraries(cpp_sdk_smoke_test PRIVATE axtp_sdk)
    add_test(NAME cpp_sdk_smoke_test COMMAND cpp_sdk_smoke_test)
endif()

if(TARGET axtpctl)
    add_test(NAME axtpctl_help COMMAND axtpctl --help)
    add_test(NAME axtpctl_capability_methods COMMAND axtpctl capability methods)
    add_test(NAME axtpctl_list_methods COMMAND axtpctl list-methods)

    add_test(NAME axtpctl_unknown_method
        COMMAND axtpctl -c unknown.method
    )
    set_tests_properties(axtpctl_unknown_method PROPERTIES WILL_FAIL TRUE)

    add_test(NAME axtpctl_json_conflict
        COMMAND axtpctl -c audio.setAlgorithmConfig
            --json {}
            --json-file ${AXTP_CPP_RUNTIME_ROOT}/tests/tools/axtpctl/params_algorithm_config.json
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
endif()

if(TARGET axtp_stream)
    add_executable(axtp_stream_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/stream/stream_test.cpp
    )

    target_link_libraries(axtp_stream_test PRIVATE
        axtp_stream
    )

    add_test(NAME axtp_stream_test COMMAND axtp_stream_test)
endif()

if(TARGET axtp_media_profile)
    add_executable(axtp_media_profile_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/profiles/media_profile_test.cpp
    )

    target_link_libraries(axtp_media_profile_test PRIVATE
        axtp_media_profile
    )

    add_test(NAME axtp_media_profile_test COMMAND axtp_media_profile_test)
endif()

if(TARGET axtp-mediahost)
    add_test(NAME axtp_mediahost_help COMMAND axtp-mediahost --help)
    add_test(NAME axtp_mediahost_missing_hid
        COMMAND axtp-mediahost --path __axtp_missing_hid__ --timeout 1
    )
    set_tests_properties(axtp_mediahost_missing_hid PROPERTIES WILL_FAIL TRUE)
    add_test(NAME axtp_mediahost_render_missing_hid
        COMMAND axtp-mediahost --render --path __axtp_missing_hid__ --timeout 1
    )
    set_tests_properties(axtp_mediahost_render_missing_hid PROPERTIES WILL_FAIL TRUE)
endif()
