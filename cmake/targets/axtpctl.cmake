if(NOT TARGET axtp_toolkit)
    message(FATAL_ERROR "axtpctl requires axtp_toolkit")
endif()

add_executable(axtpctl
    ${AXTP_CPP_RUNTIME_ROOT}/tools/axtpctl/src/main.cpp
)

target_include_directories(axtpctl
    PRIVATE
        ${AXTP_CPP_RUNTIME_ROOT}/third_party/websocketpp
)

target_link_libraries(axtpctl
    PRIVATE
        axtp_firmware_profile
        axtp_toolkit
)
