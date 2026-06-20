if(NOT TARGET axtp_runtime OR NOT TARGET axtp_sdk)
    message(FATAL_ERROR "axtp_mediahost_protocol requires runtime and SDK targets")
endif()

add_library(axtp_mediahost_protocol INTERFACE)
target_include_directories(axtp_mediahost_protocol INTERFACE
    ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src
)
target_link_libraries(axtp_mediahost_protocol INTERFACE
    axtp_runtime
    axtp_sdk
)

if(AXTP_CPP_RUNTIME_BUILD_MEDIAHOST)
    if(NOT WIN32)
        message(FATAL_ERROR "axtp-mediahost is a Windows-only MediaHost tool.")
    endif()
    if(NOT TARGET axtp_toolkit OR NOT TARGET axtp_transport_hidapi)
        message(FATAL_ERROR "axtp-mediahost requires toolkit and HID transport targets")
    endif()

    set(AXTP_MEDIAHOST_RENDER_WIN32_PRIVATE_SOURCES
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/audio/adts_parser.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/audio/axtp_audio_renderer.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/audio/wasapi_aac_renderer.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/common/media_render_common.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/common/media_render_host_impl.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/ui/overlay_controls.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/video/axtp_video_renderer.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/video/d3d_h264_renderer.inc
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/video/h264_bitstream.inc
    )
    set_source_files_properties(${AXTP_MEDIAHOST_RENDER_WIN32_PRIVATE_SOURCES}
        PROPERTIES HEADER_FILE_ONLY TRUE
    )

    add_library(axtp_mediahost_render_win32 STATIC
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/media/render/win32/media_render_host.cpp
        ${AXTP_MEDIAHOST_RENDER_WIN32_PRIVATE_SOURCES}
    )

    target_include_directories(axtp_mediahost_render_win32 PUBLIC
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src
    )

    target_link_libraries(axtp_mediahost_render_win32
        PRIVATE
            axtp_mediahost_protocol
            avrt
            d3d11
            dxgi
            user32
            gdi32
            mf
            mfplat
            mfreadwrite
            mfuuid
            mmdevapi
            ole32
            wmcodecdspuuid
    )

    add_executable(axtp-mediahost
        ${AXTP_CPP_RUNTIME_ROOT}/tools/axtp-mediahost/src/app/main.cpp
    )

    target_link_libraries(axtp-mediahost
        PRIVATE
            axtp_mediahost_protocol
            axtp_mediahost_render_win32
            axtp_toolkit
            axtp_sdk
            axtp_transport_hidapi
    )
endif()
