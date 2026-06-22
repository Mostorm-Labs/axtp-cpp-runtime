if(NOT TARGET axtp_stream OR NOT TARGET axtp_broker)
    message(FATAL_ERROR "axtp_media_profile requires axtp_stream and axtp_broker")
endif()

add_library(axtp_media_profile INTERFACE)
target_include_directories(axtp_media_profile
    INTERFACE
        $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(axtp_media_profile INTERFACE
    axtp_stream
    axtp_broker
)
target_compile_features(axtp_media_profile INTERFACE cxx_std_17)
set_target_properties(axtp_media_profile PROPERTIES EXPORT_NAME media_profile)

if(NOT TARGET axtp::media_profile)
    add_library(axtp::media_profile ALIAS axtp_media_profile)
endif()
