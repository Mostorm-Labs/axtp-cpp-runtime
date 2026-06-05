# AXTP C++ 代码风格指南

## 1. 总体风格

AXTP C++ 采用偏 library 的代码纪律，借鉴 Skia 的清晰边界，但命名规则按 AXTP 自己的约定执行：

- public API 边界清晰，include 关系可控。
- 4 spaces，100 columns，K&R braces，不使用 tabs。
- ownership 显式，runtime API 适合 ManualPoll。
- core 和 generated traits 保持 header-only。
- cpp/core public headers 不泄漏平台依赖。
- concrete transports 作为 optional runtime/tool adapter。

AXTP 不使用 Skia 的 `fMember` 或 `gGlobal` 命名，也不要求所有类型都带 `Axtp` 前缀。AXTP core style 也不采用 obs-websocket 的 tab、132-column 或 application-plugin 风格。

`clang-format` 负责 whitespace、indentation、braces、wrapping 和 include grouping。identifier naming 和 file naming 由 review 与本文档约束；后续可补 clang-tidy naming check 或自定义 file-name lint。

## 1.1 C++ 开发文档地图

修改 C++ runtime、SDK、CLI 或 transport 代码时，按需一起阅读这些文档：

| 文档 | 范围 |
|---|---|
| `docs/AXTP_CORE_API_DESIGN.md` | Core public API、target map、分层边界 |
| `docs/AXTP_CPP_RUNTIME_PATTERNS.md` | Runtime 设计模式、extension recipe、anti-pattern、测试地图 |
| `docs/AXTP_CPP_EXECUTION_FLOW.md` | Runtime、SDK、CLI、HID、direct-core 执行流程 |
| `docs/AXTP_SDK_API_DESIGN.md` | SDK public API 和 dynamic RPC 策略 |
| `docs/AXTPCTL_COMMAND_DESIGN.md` | CLI command shape 和 command dispatch 策略 |

## 2. Namespace 与类型命名

所有 C++ symbol 都位于 `namespace axtp` 或子 namespace，例如 `axtp::sdk`。不要用全局 `Axtp` 前缀替代 namespace。

协议栈主入口保留前缀：

```cpp
axtp::AxtpCore core;
```

不要改成 `Core`；这个名字在用户代码和文档里过于泛化。

轻量 broker 是：

```cpp
axtp::BasicBroker<> broker;
```

不要把 header-only broker 命名为 `AxtpBroker`。当前 runtime 有意不保留旧的 `AxtpBroker`、`AxtpInboundProcessor` 或 `AxtpOutboundProcessor` alias。

内部 pipeline component 不带 `Axtp` 前缀：

```cpp
axtp::InboundProcessor;
axtp::OutboundProcessor;
axtp::FrameDecoder;
axtp::FrameEncoder;
axtp::MessageReassembler;
axtp::MessageFragmenter;
axtp::PayloadDecoder;
axtp::PayloadEncoder;
axtp::JsonRpcDecoder;
axtp::JsonRpcEncoder;
```

协议 model 和 runtime value type 也不带前缀：

```cpp
axtp::Frame;
axtp::Message;
axtp::RpcPayload;
axtp::ControlPayload;
axtp::StreamPayload;
axtp::PayloadMeta;
axtp::TransportProfile;
axtp::SessionContext;
axtp::BrokerTask;
axtp::BrokerResult;
```

Interface 使用 `I` 前缀，但不加 `Axtp` 前缀：

```cpp
axtp::ITransport;
axtp::IByteSink;
axtp::IByteWriter;
axtp::IPayloadSink;
```

函数名使用 lowerCamelCase：`attachTransport()`、`detachTransport()`、`flushOutbound()`、`onBytes()`、`sendBytes()`、`handleBrokerResult()`。

常量使用 `kConstantName`。新的非 generated enum value 优先采用 `kValue` 风格；已有 generated/protocol enum value，例如 `RpcOp::Request` 和 `ErrorCode::Success`，在本轮迁移中保持不变。

## 3. Private Member 命名

Private/protected non-static data member 使用 leading underscore + lowerCamelCase：

```cpp
class InboundProcessor {
private:
    PayloadDecoder _payloadDecoder;
    MessageReassembler _messageReassembler;
    AxtpWireMode _wireMode = AxtpWireMode::FramedBinary;
};
```

不要使用 `member_`、`fMember`、`gGlobal`、`__member` 或 `_Member`。

Leading underscore 安全规则：

- `_member` 只允许用于 private/protected class 或 struct data member。
- `_` 后的第一个字符必须是 lowercase。
- namespace/global scope 不使用 leading underscore identifier。
- 不使用 leading underscore macro 或 include guard，例如 `_AXTP_CORE_HPP`。
- 优先使用 `#pragma once`；如果必须用 include guard，使用非保留名称，例如 `AXTP_CORE_HPP`。

## 4. 文件命名

C++ 文件名使用 lower_snake_case。新的 C++ header 使用 `.hpp`，实现文件使用 `.cpp`。

推荐示例：

```text
axtp_core.hpp
inbound_processor.hpp
frame_decoder.hpp
message_reassembler.hpp
basic_broker.hpp
hid_transport.cpp
```

不要使用 `AxtpCore.h` 或 `AxtpFrameDecoder.hpp` 这类 UpperCamelCase 文件名。

目录也使用 lower_snake_case。如果目录已经表达模块，不要在文件名里重复模块名：

```text
core/axtp_core.hpp
core/inbound/frame_decoder.hpp
core/outbound/frame_encoder.hpp
broker/basic_broker.hpp
transport/transport.hpp
transport/transport_profile.hpp
testing/mock_transport.hpp
```

Generated files 可以继续使用当前 `.h` 文件名，直到 generator 单独负责迁移。Third-party code 不由 AXTP 脚本改名或格式化。

## 5. Include 规则

使用完整 module include path：

```cpp
#include <axtp.hpp>
#include <core/axtp_core.hpp>
#include <broker/basic_broker.hpp>
#include <core/inbound/frame_decoder.hpp>
#include <core/outbound/frame_encoder.hpp>
#include <transport/transport.hpp>
```

不要包含旧的 top-level pipeline path：

```cpp
#include <inbound/frame_decoder.hpp>   // forbidden
#include <outbound/frame_encoder.hpp>  // forbidden
```

Core public headers 可以 include 标准 C++ 头、AXTP public headers、generated headers，以及正式 `WebSocketJsonRpc` core wire mode 需要的 Boost.JSON。它们不能 include `windows.h`、`unistd.h`、`sys/socket.h`、`hidapi.h`、Boost.Asio、Boost.Beast、websocket libraries、Qt headers 或 pthread headers。

Concrete transport target 和 tool 可以 include 平台库，但这些依赖不能通过 cpp/core public headers 泄漏。

## 6. Runtime 分层与 Ownership

Runtime 分层固定为：

```text
ITransport <-> AxtpEndpoint -> AxtpCore -> BasicBroker
```

- `AxtpEndpoint` 是唯一 glue layer。
- `AxtpCore` 可以知道 payload models、processors、`CoreEvent`、`BrokerResult` 和 `TransportProfile`。
- `AxtpCore` 不得知道 `ITransport`、concrete transport、SDK client、CLI code 或 legacy command id。
- `BasicBroker<>` 可以知道 `BrokerTask`、`BrokerResult`、method registry 和 handler dispatch。
- `BasicBroker<>` 不得知道 `AxtpCore`、`ITransport`、frame 或 transport-specific behavior。

Transport implementation 以非 owning callback target 的方式保存 `IByteSink*`。`AxtpEndpoint` 不拥有 transport，也不拥有 broker；应用层或 SDK 负责 ownership。

推荐生命周期：

```text
construct -> attach/bind -> open -> poll -> close -> detach -> destroy
```

不要制造 ownership cycle。

## 7. Header-Only Core、Broker 与 Transport 规则

`axtp_core` 和 `BasicBroker<>` 是 header-only、ManualPoll-based。

Core 可以解析 frame、重组 message、解码 payload envelope、维护 session、跟踪 pending call、排队 outbound bytes。Core 不得创建 thread、做 socket I/O、调用 HID API、直接执行业务 handler 或硬编码 legacy command ID。

`BasicBroker<>` 按 `RpcPayload.methodOrEventId` 和 raw body bytes 分发业务 handler。它不得引入 `std::thread`、condition variable、future、platform executor 或 socket/transport 行为。

`ITransport` 属于 core interface。真实 transport implementation 是可选 runtime/tool/platform code。Transport 不能解析 AXTP frame、payload、method ID 或 legacy command ID；它只负责读写 bytes 或 transport-native message。

## 8. 格式化脚本

在 `axtp-cpp-runtime` 仓库中使用：

```bash
scripts/format-cpp.sh
scripts/check-format-cpp.sh
```

脚本扫描 `core`、`sdk`、`json-rpc`、`transports` 和 `tools`，排除 `build/`、`generated/` 和 `thirdparty/`。
