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

    set(AXTP_HIDAPI_TARGET "" PARENT_SCOPE)
endfunction()
