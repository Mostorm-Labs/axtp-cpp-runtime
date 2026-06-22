if(NOT TARGET axtp_core)
    message(FATAL_ERROR "axtp_stream requires axtp_core")
endif()

add_library(axtp_stream INTERFACE)
target_include_directories(axtp_stream
    INTERFACE
        $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(axtp_stream INTERFACE
    axtp_core
)
target_compile_features(axtp_stream INTERFACE cxx_std_17)
set_target_properties(axtp_stream PROPERTIES EXPORT_NAME stream)

if(NOT TARGET axtp::stream)
    add_library(axtp::stream ALIAS axtp_stream)
endif()
