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
`ITransport` 抽象，不需要手动配置 AXTP include 路径。cpp-runtime 不再提供
TCP、HID、WebSocket concrete provider；应用负责从 Axent 或自己的 provider
target 选择实现并注入 runtime。

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

默认安装包会导出 `axtp::core`、`axtp::broker`、`axtp::runtime` 和
`axtp::sdk`；启用 JSON-RPC 时还会导出 `axtp::json_rpc`。core/SDK 中的
`StreamPayload` wire 能力继续保留，但不导出 Host 型 stream
registry/coordinator 或 concrete transport target。如果仓内
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

## Concrete Transport Providers

cpp-runtime 只保留 `ITransport`、`TransportProfile` 和 endpoint attach/bind
契约。TCP、WebSocket、HID 等 concrete provider 属于 Axent 或嵌入应用。
应用同时链接 runtime SDK 与自己的 provider，然后显式调用
`attachTransport()`：

```cmake
add_subdirectory(third_party/axent)

target_link_libraries(your_app PRIVATE
    axtp::sdk
    axent::transport_hidapi
)
```

独立消费 cpp-runtime、但不使用 Axent 的项目，需要自行实现 `ITransport`。
cpp-runtime 不解析或下载 hidapi、IXWebSocket、Boost.Asio/Beast 等 provider
依赖。

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
| `AXTP_BUILD_JSON_RPC` | `OFF` | 打开可选 `axtp::json_rpc` helper target。 |

## Common Pitfalls

- 不要从外部项目手动拼 `include_directories()` 或把 `include/` 写进
  `#include` 前缀；link target 后使用 `<axtp_sdk.hpp>`、
  `<axtp_runtime.hpp>`、`<axtp_core.hpp>` 这类入口，或按需使用
  `<core/...>`、`<sdk/...>`、`<json_rpc/...>`。
- 需要 TCP、HID 或 WebSocket 时，链接 Axent 或应用自有 provider；不要期待
  cpp-runtime 定义 concrete transport target。
- 不要手写 `include/core/protocol/generated/**`。协议事实由锁定的 AXTP spec
  和 generator 生成。
