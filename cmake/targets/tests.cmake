add_test(
    NAME runtime_layer_boundary_docs
    COMMAND ${CMAKE_COMMAND}
        -DAXTP_CPP_RUNTIME_ROOT=${AXTP_CPP_RUNTIME_ROOT}
        -P ${AXTP_CPP_RUNTIME_ROOT}/tests/cmake/verify_layer_boundaries.cmake
)

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
target_include_directories(phase4_core_test PRIVATE
    ${AXTP_CPP_RUNTIME_ROOT}/devtools/conformance)
target_link_libraries(phase4_core_test PRIVATE axtp_core)
add_test(NAME phase4_core_test COMMAND phase4_core_test)

add_executable(phase5_transport_test
    ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase5_transport_test.cpp
)
target_link_libraries(phase5_transport_test PRIVATE axtp_core)
add_test(NAME phase5_transport_test COMMAND phase5_transport_test)

if(TARGET axtp_json_rpc)
    add_executable(phase6_json_rpc_session_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/core/phase6_json_rpc_session_test.cpp
    )
    target_link_libraries(phase6_json_rpc_session_test
        PRIVATE axtp_core axtp_json_rpc)
    add_test(NAME phase6_json_rpc_session_test COMMAND phase6_json_rpc_session_test)
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

if(TARGET axtp_sdk)
    add_executable(cpp_sdk_smoke_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/sdk/sdk_smoke_test.cpp
    )
    target_link_libraries(cpp_sdk_smoke_test PRIVATE axtp_sdk)
    add_test(NAME cpp_sdk_smoke_test COMMAND cpp_sdk_smoke_test)

    add_executable(cpp_sdk_call_options_test
        ${AXTP_CPP_RUNTIME_ROOT}/tests/sdk/call_options_test.cpp
    )
    target_link_libraries(cpp_sdk_call_options_test PRIVATE axtp_sdk)
    target_compile_definitions(cpp_sdk_call_options_test PRIVATE AXTP_RUNTIME_TESTING=1)
    add_test(NAME cpp_sdk_call_options_test COMMAND cpp_sdk_call_options_test)
endif()
