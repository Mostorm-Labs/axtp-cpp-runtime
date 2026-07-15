cmake_minimum_required(VERSION 3.16)

foreach(AXTP_CPP_RUNTIME_RETIRED_TRANSPORT_OPTION
        AXTP_BUILD_OPTIONAL_TRANSPORTS
        AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS)
    if(DEFINED ${AXTP_CPP_RUNTIME_RETIRED_TRANSPORT_OPTION})
        message(FATAL_ERROR
            "${AXTP_CPP_RUNTIME_RETIRED_TRANSPORT_OPTION} was removed with the "
            "cpp-runtime concrete transport wrappers. Provide an ITransport "
            "implementation from Axent or the embedding application instead.")
    endif()
endforeach()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AXTP_CPP_RUNTIME_VERSION LIMIT_COUNT 1)
project(axtp_cpp_runtime VERSION ${AXTP_CPP_RUNTIME_VERSION} LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(AXTP_CPP_RUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(AXTP_CPP_RUNTIME_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

set(AXTP_CPP_RUNTIME_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(AXTP_CPP_RUNTIME_IS_TOP_LEVEL ON)
endif()

option(AXTP_CPP_RUNTIME_BUILD_SDK "Build the AXTP SDK target" ON)
option(AXTP_CPP_RUNTIME_BUILD_TESTS "Build AXTP C++ runtime tests" ${AXTP_CPP_RUNTIME_IS_TOP_LEVEL})
option(AXTP_CPP_RUNTIME_BUILD_CONFORMANCE "Build AXTP conformance runner" OFF)
option(AXTP_CPP_RUNTIME_ENABLE_INSTALL "Install AXTP C++ runtime package targets" ${AXTP_CPP_RUNTIME_IS_TOP_LEVEL})
option(AXTP_BUILD_JSON_RPC "Build optional AXTP WebSocket JSON-RPC helpers" OFF)

if(AXTP_CPP_RUNTIME_BUILD_CONFORMANCE)
    set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
endif()

if(AXTP_CPP_RUNTIME_BUILD_TESTS)
    enable_testing()
endif()

include(cmake/targets/core.cmake)

set(AXTP_CPP_RUNTIME_NEEDS_SDK ${AXTP_CPP_RUNTIME_BUILD_SDK})
if(AXTP_CPP_RUNTIME_BUILD_TESTS)
    set(AXTP_CPP_RUNTIME_NEEDS_SDK ON)
endif()

if(NOT AXTP_CPP_RUNTIME_BUILD_SDK AND AXTP_CPP_RUNTIME_NEEDS_SDK)
    message(STATUS "Enabling axtp_sdk target because tests require it")
endif()

if(AXTP_CPP_RUNTIME_NEEDS_SDK)
    include(cmake/targets/sdk.cmake)
endif()

if(AXTP_CPP_RUNTIME_BUILD_CONFORMANCE)
    if(NOT TARGET axtp_json_rpc)
        message(FATAL_ERROR "AXTP conformance runner requires AXTP_BUILD_JSON_RPC")
    endif()

    add_executable(axtp_conformance_runner
        devtools/conformance/conformance_runner.cpp
    )
    target_include_directories(axtp_conformance_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/devtools/conformance)
    target_link_libraries(axtp_conformance_runner PRIVATE axtp_core axtp_json_rpc)
    target_compile_definitions(axtp_conformance_runner PRIVATE
        AXTP_CONFORMANCE_YAML_READER="${CMAKE_CURRENT_SOURCE_DIR}/devtools/conformance/yaml_to_json.rb")
endif()

if(AXTP_CPP_RUNTIME_BUILD_TESTS)
    include(tests/cmake/verify_target_boundaries.cmake)
    include(cmake/targets/tests.cmake)
endif()

if(AXTP_CPP_RUNTIME_ENABLE_INSTALL)
    set(AXTP_CPP_RUNTIME_INSTALL_TARGETS
        axtp_core
        axtp_broker
        axtp_runtime
    )
    if(TARGET axtp_sdk)
        list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_sdk)
    endif()
    if(TARGET axtp_json_rpc)
        list(APPEND AXTP_CPP_RUNTIME_INSTALL_TARGETS axtp_json_rpc)
    endif()

    install(TARGETS ${AXTP_CPP_RUNTIME_INSTALL_TARGETS}
        EXPORT axtp_cpp_runtimeTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY
            include/core
            include/sdk
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(FILES
            include/axtp_core.hpp
            include/axtp_runtime.hpp
            include/axtp_sdk.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    if(TARGET axtp_json_rpc)
        install(DIRECTORY include/json_rpc
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
        install(FILES include/axtp_json_rpc.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()
    set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON ON)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/include/nlohmann/json.hpp")
        set(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON OFF)
        install(DIRECTORY third_party/json/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()

    set(AXTP_CPP_RUNTIME_CONFIG_FIND_NLOHMANN_JSON "")
    if(AXTP_CPP_RUNTIME_CONFIG_NEEDS_NLOHMANN_JSON)
        set(AXTP_CPP_RUNTIME_CONFIG_FIND_NLOHMANN_JSON
            "find_dependency(nlohmann_json CONFIG REQUIRED)")
    endif()

    set(AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR
        ${CMAKE_INSTALL_LIBDIR}/cmake/axtp_cpp_runtime
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/axtp_cpp_runtimeConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfig.cmake
        INSTALL_DESTINATION ${AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR}
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(EXPORT axtp_cpp_runtimeTargets
        NAMESPACE axtp::
        DESTINATION ${AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR}
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/axtp_cpp_runtimeConfigVersion.cmake
        DESTINATION ${AXTP_CPP_RUNTIME_INSTALL_CMAKEDIR}
    )
endif()
