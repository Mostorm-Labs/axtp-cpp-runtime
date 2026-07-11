function(axtp_cpp_runtime_select_hidapi_target output_variable)
    foreach(candidate
            hidapi::hidapi
            hidapi::hidapi-shared
            hidapi::hidapi-static
            hidapi::hidapi-darwin
            hidapi::hidapi-hidraw
            hidapi::hidapi-libusb
            hidapi::darwin
            hidapi::hidraw
            hidapi::libusb
            PkgConfig::HIDAPI
            PkgConfig::HIDAPI_HIDRAW
            PkgConfig::HIDAPI_LIBUSB)
        if(TARGET ${candidate})
            set(${output_variable} ${candidate} PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${output_variable} "" PARENT_SCOPE)
endfunction()

function(axtp_cpp_runtime_resolve_ixwebsocket)
    if(TARGET ixwebsocket::ixwebsocket)
        return()
    endif()

    if(TARGET ixwebsocket)
        add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
        return()
    endif()

    find_package(ixwebsocket CONFIG QUIET)
    if(TARGET ixwebsocket::ixwebsocket)
        return()
    endif()

    if(NOT AXTP_CPP_RUNTIME_TOOLS_FETCH_DEPS)
        return()
    endif()

    set(AXTP_THIRDPARTY_IXWEBSOCKET_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/IXWebSocket)
    if(EXISTS "${AXTP_THIRDPARTY_IXWEBSOCKET_DIR}/CMakeLists.txt")
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
        set(USE_TLS OFF CACHE BOOL "" FORCE)
        set(USE_ZLIB OFF CACHE BOOL "" FORCE)
        set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            ${AXTP_THIRDPARTY_IXWEBSOCKET_DIR}
            ${AXTP_CPP_RUNTIME_BINARY_DIR}/third_party/IXWebSocket
            EXCLUDE_FROM_ALL
        )
    else()
        include(FetchContent)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
        set(USE_TLS OFF CACHE BOOL "" FORCE)
        set(USE_ZLIB OFF CACHE BOOL "" FORCE)
        set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            axtp_tool_ixwebsocket
            GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
            GIT_TAG 2efe037c9cc96fd536774f17bdb5215161ee5087
        )
        FetchContent_MakeAvailable(axtp_tool_ixwebsocket)
    endif()
endfunction()

function(axtp_cpp_runtime_resolve_hidapi)
    axtp_cpp_runtime_select_hidapi_target(AXTP_SELECTED_HIDAPI_TARGET)
    if(AXTP_SELECTED_HIDAPI_TARGET)
        set(AXTP_HIDAPI_TARGET ${AXTP_SELECTED_HIDAPI_TARGET} PARENT_SCOPE)
        return()
    endif()

    find_package(hidapi CONFIG QUIET)
    axtp_cpp_runtime_select_hidapi_target(AXTP_SELECTED_HIDAPI_TARGET)
    if(AXTP_SELECTED_HIDAPI_TARGET)
        set(AXTP_HIDAPI_TARGET ${AXTP_SELECTED_HIDAPI_TARGET} PARENT_SCOPE)
        return()
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(HIDAPI QUIET IMPORTED_TARGET hidapi)
        if(NOT TARGET PkgConfig::HIDAPI)
            pkg_check_modules(HIDAPI_HIDRAW QUIET IMPORTED_TARGET hidapi-hidraw)
        endif()
        if(NOT TARGET PkgConfig::HIDAPI AND
           NOT TARGET PkgConfig::HIDAPI_HIDRAW)
            pkg_check_modules(HIDAPI_LIBUSB QUIET IMPORTED_TARGET hidapi-libusb)
        endif()
    endif()

    axtp_cpp_runtime_select_hidapi_target(AXTP_SELECTED_HIDAPI_TARGET)
    if(AXTP_SELECTED_HIDAPI_TARGET)
        set(AXTP_HIDAPI_TARGET ${AXTP_SELECTED_HIDAPI_TARGET} PARENT_SCOPE)
        return()
    endif()

    if(NOT AXTP_CPP_RUNTIME_TOOLS_FETCH_DEPS)
        set(AXTP_HIDAPI_TARGET "" PARENT_SCOPE)
        return()
    endif()

    set(AXTP_THIRDPARTY_HIDAPI_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/hidapi)
    if(EXISTS "${AXTP_THIRDPARTY_HIDAPI_DIR}/CMakeLists.txt")
        set(HIDAPI_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
        set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
        set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            ${AXTP_THIRDPARTY_HIDAPI_DIR}
            ${AXTP_CPP_RUNTIME_BINARY_DIR}/third_party/hidapi
            EXCLUDE_FROM_ALL
        )
    else()
        include(FetchContent)
        set(HIDAPI_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
        set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
        set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            axtp_tool_hidapi
            GIT_REPOSITORY https://github.com/libusb/hidapi.git
            GIT_TAG c3509c11174fe80ff59a47119433a7db5299af85
        )
        FetchContent_MakeAvailable(axtp_tool_hidapi)
    endif()

    axtp_cpp_runtime_select_hidapi_target(AXTP_SELECTED_HIDAPI_TARGET)
    set(AXTP_HIDAPI_TARGET ${AXTP_SELECTED_HIDAPI_TARGET} PARENT_SCOPE)
endfunction()
