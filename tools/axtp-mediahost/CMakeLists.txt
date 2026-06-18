cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(axtp_mediahost LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "axtp-mediahost is a Windows-only MediaHost tool.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT TARGET axtp_sdk)
    set(AXTP_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
    set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../sdk ${CMAKE_CURRENT_BINARY_DIR}/sdk)
endif()

add_library(axtp_mediahost_protocol INTERFACE)
target_include_directories(axtp_mediahost_protocol INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(axtp_mediahost_protocol INTERFACE
    axtp_runtime
    axtp_sdk
)

set(AXTP_MEDIAHOST_RENDER_WIN32_PRIVATE_SOURCES
    src/media/render/win32/audio/adts_parser.inc
    src/media/render/win32/audio/axtp_audio_renderer.inc
    src/media/render/win32/audio/wasapi_aac_renderer.inc
    src/media/render/win32/common/media_render_common.inc
    src/media/render/win32/common/media_render_host_impl.inc
    src/media/render/win32/ui/overlay_controls.inc
    src/media/render/win32/video/axtp_video_renderer.inc
    src/media/render/win32/video/d3d_h264_renderer.inc
    src/media/render/win32/video/h264_bitstream.inc
)
set_source_files_properties(${AXTP_MEDIAHOST_RENDER_WIN32_PRIVATE_SOURCES}
    PROPERTIES HEADER_FILE_ONLY TRUE
)

add_library(axtp_mediahost_render_win32 STATIC
    src/media/render/win32/media_render_host.cpp
    ${AXTP_MEDIAHOST_RENDER_WIN32_PRIVATE_SOURCES}
)

target_include_directories(axtp_mediahost_render_win32 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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
    src/app/main.cpp
)

target_link_libraries(axtp-mediahost
    PRIVATE
        axtp_mediahost_protocol
        axtp_mediahost_render_win32
        axtp_sdk
        axtp_transport_hidapi
)

enable_testing()

add_test(NAME axtp_mediahost_help COMMAND axtp-mediahost --help)
add_test(NAME axtp_mediahost_missing_hid COMMAND axtp-mediahost --timeout 1)
set_tests_properties(axtp_mediahost_missing_hid PROPERTIES WILL_FAIL TRUE)
add_test(NAME axtp_mediahost_render_missing_hid COMMAND axtp-mediahost --render --timeout 1)
set_tests_properties(axtp_mediahost_render_missing_hid PROPERTIES WILL_FAIL TRUE)

add_executable(axtp_mediahost_protocol_test
    tests/mediahost_protocol_test.cpp
)

target_include_directories(axtp_mediahost_protocol_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(axtp_mediahost_protocol_test PRIVATE
    axtp_mediahost_protocol
)

add_test(NAME axtp_mediahost_protocol_test COMMAND axtp_mediahost_protocol_test)
