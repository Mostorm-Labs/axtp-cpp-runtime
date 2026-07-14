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
      include/
      src/
      tools/
      third_party/
```

如果使用 git submodule：

```bash
git submodule add <axtp-cpp-runtime-repo-url> third_party/axtp-cpp-runtime
git submodule update --init --recursive third_party/axtp-cpp-runtime
```

`third_party/json` 是默认的 `nlohmann_json` 来源；如果没有初始化 submodule，
构建会退回到 `find_package(nlohmann_json CONFIG REQUIRED)`。

## Use The SDK

大多数应用从 SDK 开始。它会带上 `axtp_runtime`、core 头文件和
`ITransport` 抽象，不需要手动配置 AXTP include 路径。它不会自动带上 TCP、
HID、WebSocket 或 hidapi/IXWebSocket 这类 concrete transport 依赖；应用负责
选择并链接具体 transport。

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
#include <axtp_sdk.hpp>
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

默认安装包会导出 `axtp::core`、`axtp::broker`、`axtp::runtime`、
`axtp::sdk` 和 firmware profile。core/SDK 中的 `StreamPayload` wire 能力
继续保留，但不再导出 Host 型 stream registry/coordinator target。如果仓内
`third_party/json` 已初始化，安装包会一并安装
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
#include <axtp_runtime.hpp>
```

选择 target 的经验规则：

| Target | Include entry | Use when |
|---|---|---|
| `axtp::sdk` | `<axtp_sdk.hpp>` | 调业务 method、使用 `AxtpClient`/`AxtpDevice`。 |
| `axtp::runtime` | `<axtp_runtime.hpp>` | 自己 host method、使用 `AxtpEndpoint`/`BasicBroker<>`。 |
| `axtp::core` | `<axtp_core.hpp>` | 只需要协议 model、generated registry、frame/payload codec。 |
| `axtp::transport_tcp_native` | `<axtp_transport_tcp_native.hpp>` | 显式启用 optional transports 后，使用不依赖 Boost 的 native TCP transport。 |
| `axtp::transport_hidapi` | `<axtp_transport_hid.hpp>` | 显式启用 optional transports 后，使用真实 HID 设备 transport。 |

## Optional Transports

TCP、HID、Boost TCP、WebSocket 等 concrete transport 默认不在外部接入时自动
打开。需要它们时，在 `add_subdirectory` 之前设置选项，并由顶层项目提供对应
第三方依赖：

```cmake
set(AXTP_BUILD_OPTIONAL_TRANSPORTS ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/axtp-cpp-runtime)

target_link_libraries(your_app PRIVATE
    axtp::sdk
    axtp::transport_hidapi
)
```

HID 入口头文件：

```cpp
#include <axtp_transport_hid.hpp>
```

如果使用 HID，请确保顶层项目统一选择 hidapi 来源：可以先定义
`hidapi::hidapi` 等兼容 target，也可以在系统里提供能被
`find_package(hidapi CONFIG)` / pkg-config 找到的 hidapi。像 Axent 这类同时
依赖 AXDP 和 AXTP 的应用，推荐在顶层统一提供 hidapi，避免多个子项目各自拉取
不同 hidapi target。

同理，IXWebSocket transport 会优先使用顶层已经定义的
`ixwebsocket::ixwebsocket` / `ixwebsocket` target，其次使用
`find_package(ixwebsocket CONFIG)`。默认情况下，`AXTP_BUILD_OPTIONAL_TRANSPORTS`
不会下载 concrete transport 依赖；缺少依赖时，对应 transport target 不会被定义。

cpp-runtime 不再通过 FetchContent 下载 concrete transport 依赖。产品和工具应由
顶层项目统一提供 hidapi/IXWebSocket；媒体 Host 与协议 CLI 由 Axent 提供。

## Optional JSON-RPC Helpers

`include/core/protocol/wire/websocket_json_rpc/` 是协议 codec。顶层
`json-rpc/` 是 runtime 之上的可选 helper 层，提供 registry JSON 加载和
WebSocket JSON-RPC adapter/session glue。

```cmake
set(AXTP_BUILD_JSON_RPC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/axtp-cpp-runtime)

target_link_libraries(your_app PRIVATE axtp::json_rpc)
```

## CMake Options

这些选项需要在 `add_subdirectory(third_party/axtp-cpp-runtime)` 之前设置：

| Option | Default | Meaning |
|---|---:|---|
| `AXTP_CPP_RUNTIME_BUILD_SDK` | `ON` | 构建 `axtp::sdk`。设为 `OFF` 时只进入 core/runtime。 |
| `AXTP_CPP_RUNTIME_BUILD_TESTS` | top-level `ON`, subdirectory `OFF` | 构建仓内一方测试。 |
| `AXTP_CPP_RUNTIME_BUILD_CONFORMANCE` | `OFF` | 构建 conformance runner。 |
| `AXTP_CPP_RUNTIME_ENABLE_INSTALL` | top-level `ON`, subdirectory `OFF` | 生成 install/export/package config 规则。 |
| `AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS` | `OFF` | 安装已构建的 optional transport targets；需要对应外部依赖可被消费端找到。 |
| `AXTP_BUILD_JSON_RPC` | `OFF` | 打开可选 `axtp::json_rpc` helper target。 |
| `AXTP_BUILD_OPTIONAL_TRANSPORTS` | `OFF` | 打开 TCP/HID/Boost/WebSocket optional transport targets。 |

`AXTP_CPP_RUNTIME_INSTALL_OPTIONAL_TRANSPORTS` 只导出可以形成可消费安装包的
optional target。如果 IXWebSocket 或 hidapi provider 不是可独立发现的安装包，
安装规则会跳过对应 target，避免导出的 package 指向不可重建的底层第三方 target。
需要安装 HID/IX target 时，优先让消费环境提供可被 `find_package` 找到的
hidapi/ixwebsocket 包。

## Common Pitfalls

- 不要从外部项目手动拼 `include_directories()` 或把 `include/` 写进
  `#include` 前缀；link target 后使用 `<axtp_sdk.hpp>`、
  `<axtp_runtime.hpp>`、`<axtp_core.hpp>` 这类入口，或按需使用
  `<core/...>`、`<sdk/...>`、`<transports/...>`、`<json_rpc/...>`。
- 需要 TCP、HID 或 WebSocket 时，先打开 optional transport 选项，再 link 对应
  target；`axtp::sdk` 本身不会强制拉入平台 transport 依赖。
- 不要手写 `include/core/protocol/generated/**`。协议事实由锁定的 AXTP spec
  和 generator 生成。
