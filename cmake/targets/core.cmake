add_library(axtp_core INTERFACE)
target_include_directories(axtp_core
    INTERFACE
        $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set(AXTP_THIRDPARTY_JSON_DIR ${AXTP_CPP_RUNTIME_ROOT}/third_party/json)
set(AXTP_BUNDLED_JSON_AVAILABLE OFF)
if(EXISTS "${AXTP_THIRDPARTY_JSON_DIR}/CMakeLists.txt" AND NOT TARGET nlohmann_json::nlohmann_json)
    set(AXTP_BUNDLED_JSON_AVAILABLE ON)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    add_subdirectory(
        ${AXTP_THIRDPARTY_JSON_DIR}
        ${AXTP_CPP_RUNTIME_BINARY_DIR}/third_party/json
        EXCLUDE_FROM_ALL
    )
elseif(EXISTS "${AXTP_THIRDPARTY_JSON_DIR}/include/nlohmann/json.hpp")
    set(AXTP_BUNDLED_JSON_AVAILABLE ON)
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

set(AXTP_JSON_INSTALL_LINK "")
if(NOT AXTP_BUNDLED_JSON_AVAILABLE)
    set(AXTP_JSON_INSTALL_LINK "$<INSTALL_INTERFACE:nlohmann_json::nlohmann_json>")
endif()

target_link_libraries(axtp_core
    INTERFACE
        $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
        ${AXTP_JSON_INSTALL_LINK}
)
target_compile_features(axtp_core INTERFACE cxx_std_17)
set_target_properties(axtp_core PROPERTIES EXPORT_NAME core)

add_library(axtp_broker INTERFACE)
target_link_libraries(axtp_broker INTERFACE axtp_core)
target_compile_features(axtp_broker INTERFACE cxx_std_17)
set_target_properties(axtp_broker PROPERTIES EXPORT_NAME broker)

add_library(axtp_runtime INTERFACE)
target_link_libraries(axtp_runtime INTERFACE axtp_core axtp_broker)
target_compile_features(axtp_runtime INTERFACE cxx_std_17)
set_target_properties(axtp_runtime PROPERTIES EXPORT_NAME runtime)

if(NOT TARGET axtp::core)
    add_library(axtp::core ALIAS axtp_core)
endif()
if(NOT TARGET axtp::broker)
    add_library(axtp::broker ALIAS axtp_broker)
endif()
if(NOT TARGET axtp::runtime)
    add_library(axtp::runtime ALIAS axtp_runtime)
endif()
if(AXTP_BUILD_JSON_RPC)
    add_library(axtp_json_rpc INTERFACE)
    target_include_directories(axtp_json_rpc
        INTERFACE
            $<BUILD_INTERFACE:${AXTP_CPP_RUNTIME_ROOT}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(axtp_json_rpc
        INTERFACE
            axtp_core
    )
    target_compile_features(axtp_json_rpc INTERFACE cxx_std_17)
    set_target_properties(axtp_json_rpc PROPERTIES EXPORT_NAME json_rpc)
    if(NOT TARGET axtp::json_rpc)
        add_library(axtp::json_rpc ALIAS axtp_json_rpc)
    endif()
endif()
