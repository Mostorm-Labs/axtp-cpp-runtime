# AXTP C++ Third-Party Usage

这份文档面向把 `axtp-cpp-runtime` 放进其他 C++ 仓库的接入方。优先使用
CMake target，不要手动拼 `include_directories`；target 会把需要的 public
include path 和依赖关系传递给你的业务 target。

## Recommended Layout

推荐外部仓库把 runtime 固定在自己的第三方目录中：

```text
your-app/
  CMakeLists.txt
  src/
    main.cpp
  third_party/
    axtp-cpp-runtime/
      CMakeLists.txt
      core/
      sdk/
      transports/
      thirdparty/
```

如果使用 git submodule：

```bash
git submodule add <axtp-cpp-runtime-repo-url> third_party/axtp-cpp-runtime
git submodule update --init --recursive third_party/axtp-cpp-runtime
```

`thirdparty/json` 是默认的 `nlohmann_json` 来源；如果没有初始化 submodule，
构建会退回到 `find_package(nlohmann_json CONFIG REQUIRED)`。

## Use The SDK

大多数应用从 SDK 开始。它会带上 `axtp_runtime`、core 头文件和 native TCP
transport，不需要手动配置 AXTP include 路径。

仓内可运行的最小示例在 `examples/quickstart/`。

```cmake
cmake_minimum_required(VERSION 3.16)
project(your_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(third_party/axtp-cpp-runtime)

add_executable(your_app src/main.cpp)
target_link_libraries(your_app PRIVATE axtp::sdk)
```

入口头文件：

```cpp
#include "sdk/axtp_sdk_all.hpp"
```

兼容旧 target 名时也可以 link `axtp_sdk`，但新项目建议使用 `axtp::sdk`。

## Use An Installed Package

如果不想把源码作为 submodule 放进业务仓库，可以先安装 runtime，再通过
`find_package` 引用：

```bash
cmake -S axtp-cpp-runtime -B build/axtp-cpp-runtime \
  -DCMAKE_INSTALL_PREFIX=/opt/axtp-cpp-runtime
cmake --build build/axtp-cpp-runtime
cmake --install build/axtp-cpp-runtime
```

业务仓库的 `CMakeLists.txt`：

```cmake
find_package(axtp_cpp_runtime CONFIG REQUIRED)

add_executable(your_app src/main.cpp)
target_link_libraries(your_app PRIVATE axtp::sdk)
```

配置业务仓库时把安装前缀传给 CMake：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/axtp-cpp-runtime
```

默认安装包会导出 `axtp::core`、`axtp::runtime`、`axtp::transport_tcp_native`
和 `axtp::sdk`。如果仓内 `thirdparty/json` 已初始化，安装包会一并安装
`nlohmann/json.hpp`，业务仓库不需要单独安装 nlohmann_json。

## Use Core Or Runtime Only

只需要协议类型、wire codec、broker 或 endpoint 时，可以不引入 SDK：

```cmake
set(AXTP_CPP_RUNTIME_BUILD_SDK OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/axtp-cpp-runtime)

target_link_libraries(your_app PRIVATE axtp::runtime)
```

入口头文件：

```cpp
#include "axtp.hpp"
```

选择 target 的经验规则：

| Target | Include entry | Use when |
|---|---|---|
| `axtp::sdk` | `"sdk/axtp_sdk_all.hpp"` | 调业务 method、使用 `AxtpClient`/`AxtpDevice`。 |
| `axtp::runtime` | `"axtp.hpp"` | 自己 host method、使用 `AxtpEndpoint`/`BasicBroker<>`。 |
| `axtp::core` | `"axtp.hpp"` | 只需要协议 model、generated registry、frame/payload codec。 |
| `axtp::transport_tcp_native` | `"transports/tcp/native/tcp_transport.hpp"` | 使用不依赖 Boost 的 native TCP transport。 |

## Optional Transports

HID、Boost TCP、WebSocket 等 transport 默认不在外部接入时自动打开。需要它们时，
在 `add_subdirectory` 之前设置选项：

```cmake
set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/axtp-cpp-runtime)

target_link_libraries(your_app
    PRIVATE
        axtp::sdk
        axtp::transport_hidapi
)
```

HID 入口头文件：

```cpp
#include "transports/hidapi/hid_transport.hpp"
```

如果使用 HID，请确保 `thirdparty/hidapi` submodule 已初始化，或系统里能被
`find_package(hidapi CONFIG)` / pkg-config 找到。

## Optional JSON-RPC Helpers

`core/include/protocol/wire/websocket_json_rpc/` 是协议 codec。顶层
`json-rpc/` 是 runtime 之上的可选 helper 层，提供 registry JSON 加载和
WebSocket JSON-RPC adapter/session glue。

```cmake
set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/axtp-cpp-runtime)

target_link_libraries(your_app PRIVATE axtp::json_rpc)
```

## CMake Options

这些选项需要在 `add_subdirectory(third_party/axtp-cpp-runtime)` 之前设置：

| Option | Default as third-party | Meaning |
|---|---:|---|
| `AXTP_CPP_RUNTIME_BUILD_SDK` | `ON` | 根目录接入时构建 `axtp::sdk`。设为 `OFF` 时只进入 core。 |
| `AXTP_CPP_RUNTIME_BUILD_TOOLS` | `OFF` | 构建 `axtpctl`。应用接入通常不需要。 |
| `AXTP_CPP_RUNTIME_BUILD_MEDIAHOST` | `OFF` | 构建 Windows MediaHost 示例。应用接入通常不需要。 |
| `AXTP_CPP_RUNTIME_ENABLE_INSTALL` | top-level `ON` | 生成 install/export/package config 规则；作为子目录接入时默认关闭。 |
| `AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS` | `OFF` | 安装已构建的 optional transport targets；需要对应外部依赖可被消费端找到。 |
| `AXTP_BUILD_JSON_RPC` | `OFF` | 打开可选 `axtp::json_rpc` helper target。 |
| `AXTP_BUILD_OPTIONAL_TRANSPORTS` | `OFF` | 打开 HID/Boost/WebSocket optional transport targets。 |
| `AXTP_CORE_BUILD_TESTS` | `OFF` | 构建 core 测试。外部项目通常保持关闭。 |
| `AXTP_SDK_BUILD_TESTS` | `OFF` | 构建 SDK 测试。外部项目通常保持关闭。 |

`AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS` 只导出可以形成可消费安装包的
optional target。如果 IXWebSocket 或 hidapi 是从本仓 `thirdparty/` submodule
直接构建出来的，安装规则会跳过对应 target，避免导出的 package 指向未安装的
底层第三方 target。需要安装 HID/IX target 时，优先让消费环境提供可被
`find_package` 找到的 hidapi/ixwebsocket 包。

## Common Pitfalls

- 不要从外部项目直接 include `core/include/...` 或 `sdk/include/...` 作为路径前缀；
  link target 后使用 `"axtp.hpp"`、`"sdk/axtp_sdk_all.hpp"` 这类头文件名。
- 不要直接依赖 `tools/toolkit`。它只服务仓内工具，不是稳定 SDK API。
- 需要 HID 或 WebSocket 时，先打开 optional transport 选项，再 link 对应
  target；`axtp::sdk` 本身不会强制拉入平台 transport 依赖。
- 不要手写 `core/include/protocol/generated/**`。协议事实由锁定的 AXTP spec
  和 generator 生成。
