if(NOT TARGET axtp_runtime)
    message(FATAL_ERROR "axtp_sdk requires axtp_runtime")
endif()

add_library(axtp_sdk INTERFACE)

target_include_directories(axtp_sdk
    INTERFACE
        $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(axtp_sdk
    INTERFACE
        axtp_runtime
        axtp_transport_tcp_native
)
target_compile_features(axtp_sdk INTERFACE cxx_std_17)
set_target_properties(axtp_sdk PROPERTIES EXPORT_NAME sdk)

if(NOT TARGET axtp::sdk)
    add_library(axtp::sdk ALIAS axtp_sdk)
endif()
