if(NOT TARGET axtp_sdk)
    message(FATAL_ERROR "axtp_firmware_profile requires axtp_sdk")
endif()

add_library(axtp_firmware_profile INTERFACE)
target_include_directories(axtp_firmware_profile
    INTERFACE
        $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(axtp_firmware_profile INTERFACE
    axtp_sdk
)
target_compile_features(axtp_firmware_profile INTERFACE cxx_std_17)
set_target_properties(axtp_firmware_profile PROPERTIES EXPORT_NAME firmware_profile)

if(NOT TARGET axtp::firmware_profile)
    add_library(axtp::firmware_profile ALIAS axtp_firmware_profile)
endif()
