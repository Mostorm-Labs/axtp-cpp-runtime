# AXTP Core API 设计

## 概要

`axtp-cpp-runtime/core` 是协议正确性优先的 C++ runtime 层。它负责 AXTP model 类型、byte IO 接口、FramedBinary 解析/编码、WebSocketJsonRpc 文本 JSON 解析/编码、核心协议状态、transport profile 和运行时 lookup helper。

Core 不暴露面向业务的 client API，不直接依赖平台 transport，不包含 HID/TCP/WebSocket concrete transport，也不承载 legacy AXDP adapter。

当前 core 支持两条 AXTP v1 wire path：

| Wire mode | Inbound path | Outbound path |
|---|---|---|
| `FramedBinary` | `FrameDecoder -> MessageReassembler -> PayloadDecoder -> IPayloadSink` | `PayloadEncoder -> MessageFragmenter -> FrameEncoder -> IByteWriter` |
| `WebSocketJsonRpc` | complete text message -> `JsonRpcDecoder -> IPayloadSink` | `JsonRpcEncoder -> IByteWriter` |

`WebSocketJsonRpc` 是 AXTP 正式协议 profile，不是兼容层或 legacy adapter。旧协议 adapter 必须放在 cpp/core 之外，并作为可选包依赖 core 的归一化 payload 接口。

Runtime 分层固定为：

```text
ITransport <-> AxtpEndpoint -> AxtpCore -> BasicBroker
```

`AxtpEndpoint` 是唯一 glue layer。`AxtpCore` 输出 `CoreEvent`、接收 `BrokerResult`、生成 outbound bytes。`BasicBroker<>` 接收 `BrokerTask`、分发 handler、排队 `BrokerResult`。

## 目标与模块

| Target | 形态 | 职责 |
|---|---|---|
| `axtp_core` | `INTERFACE` | model、IO 接口、transport profile、FramedBinary pipeline、WebSocketJsonRpc decoder/encoder、`AxtpCore`、generated lookup helpers |
| `axtp_broker` | `INTERFACE` | `BasicBroker<>`、`BrokerTask`、`BrokerResult`、dynamic method dispatch helper |
| `axtp_runtime` | `INTERFACE` | core + broker + endpoint glue，供普通应用使用 |
| `axtp_json_rpc` | `INTERFACE` | WebSocket session helper adapter 和 JSON registry-file loader |
| `axtp_transport_hidapi` | `STATIC` optional | HID report-level transport，位于 `axtp-cpp-runtime/transports`，依赖 vendored `axtp-cpp-runtime/thirdparty/hidapi` |
| `axtp_transport_tcp_boost` | `INTERFACE` optional | Boost.Asio TCP transport，位于 `axtp-cpp-runtime/transports` |
| `axtp_transport_websocket_ix` | `INTERFACE` optional | 默认 IXWebSocket WebSocket transport，位于 `axtp-cpp-runtime/transports` |
| `axtp_transport_websocket_websocketpp` | `INTERFACE` optional | 高级可选 websocketpp + standalone Asio WebSocket transport，位于 `axtp-cpp-runtime/transports` |
| `axtp_transport_websocket_boost` | `INTERFACE` optional | Legacy optional Boost.Beast WebSocket transport，位于 `axtp-cpp-runtime/transports` |

推荐 runtime 聚合 include：

```cpp
#include <axtp.hpp>
```

该聚合头只包含 runtime/core/broker 常用入口，不包含 HID/TCP/WebSocket concrete transport、`MockTransport` 或 schema-aware typed codec。

## 相关开发文档

| 文档 | 作用 |
|---|---|
| `docs/AXTP_CPP_RUNTIME_PATTERNS.md` | Runtime 架构、target map、设计模式、扩展 recipe、anti-pattern |
| `docs/AXTP_CPP_EXECUTION_FLOW.md` | FramedBinary、WebSocketJsonRpc、SDK、CLI、HID、direct-core 执行流程 |
| `docs/AXTP_CPP_STYLE.md` | C++ 命名、文件布局、include、格式化和分层边界 |
| `docs/AXTP_SDK_API_DESIGN.md` | SDK API 形态和 dynamic RPC 策略 |
| `docs/AXTPCTL_COMMAND_DESIGN.md` | CLI 命令形态和 dispatch 策略 |

## Public API 分组

Core public headers 按职责分组：

| 区域 | 作用 |
|---|---|
| `model/*` | Bytes、status/result、frame、message、payload、协议常量和枚举 |
| `io/*` | byte sink/writer、可选 text writer、transport packet 边界类型 |
| `core/inbound/*` | FramedBinary 和 WebSocketJsonRpc inbound processor |
| `core/outbound/*` | FramedBinary 和 WebSocketJsonRpc outbound processor |
| `core/*` | `AxtpCore`、`CoreEvent`、session helper、pending call |
| `transport/*` | transport interface 和 profile |
| `broker/*` | `BasicBroker<>`、task dispatch、business routing、result queue |
| `runtime/*` | `AxtpEndpoint` glue 和 endpoint ports |
| `generated/*` | 生成的 ID、dynamic registry、lookup helper |

## Core API 契约

`AxtpCore` 可以脱离 endpoint、broker、SDK 和 transport 单独使用：

```cpp
axtp::AxtpCore core;
core.configure(profile);
core.byteSink().onBytes(data, size);

while (auto event = core.pollEvent()) {
    // Convert CoreEvent to your scheduler or broker.
}

core.handleBrokerResult(result);

while (auto bytes = core.tryPopOutboundBytes()) {
    writer.writeBytes(bytes->data(), bytes->size());
}
```

稳定 core 操作：

| API | 职责 |
|---|---|
| `configure(profile)` | 根据 `TransportProfile` 选择 wire mode 和 frame sizing |
| `byteSink()` | 返回 transport/adapter bytes 的 ingress port |
| `pollEvent()` | 弹出归一化协议事件，供 broker 或应用路由 |
| `handleBrokerResult(result)` | 把业务结果、event、stream output 送回 core |
| `tryPopOutboundBytes()` | 弹出按当前 wire mode 编码后的 outbound bytes |
| `expectRpcResponse(requestId)` | 登记客户端侧 pending RPC request |
| `tryTakeRpcResponse(requestId)` | 读取已完成的客户端侧 RPC response |

`AxtpCore` 不提供 `attachTransport()` 或 `attachBroker()`。这些 wiring 只属于 `AxtpEndpoint`。

## IO 边界

现有 byte-stream 接口是稳定基线：

```cpp
class IByteSink {
public:
    virtual ~IByteSink() = default;
    virtual void onBytes(const Byte* data, std::size_t size) = 0;
};

class IByteWriter {
public:
    virtual ~IByteWriter() = default;
    virtual void writeBytes(const Byte* data, std::size_t size) = 0;
};
```

为 message-oriented transport 预留的附加接口：

```cpp
class ITextWriter {
public:
    virtual ~ITextWriter() = default;
    virtual void writeText(const char* data, std::size_t size) = 0;
};

enum class TransportPacketKind {
    ByteStreamChunk,
    BinaryMessage,
    TextMessage,
};

struct TransportPacket {
    TransportPacketKind kind = TransportPacketKind::ByteStreamChunk;
    const Byte* data = nullptr;
    std::size_t size = 0;
};

class ITransportSink {
public:
    virtual ~ITransportSink() = default;
    virtual void onTransportPacket(const TransportPacket& packet) = 0;
};
```

P0 中 `WebSocketJsonRpc` 复用 `IByteSink::onBytes()`，但每次调用必须包含一条完整 UTF-8 WebSocket text message。

## Dynamic RPC 优先

Core 把 RPC body 当作 bytes 处理，`RpcEncoding` 描述这些 bytes 的语义：

- `Json`：UTF-8 JSON 业务对象 bytes。
- `Tlv`：AXTP TLV 业务 body bytes。
- `Raw`：不解释的业务 bytes。
- `Binary`：兼容期旧名，新代码应避免继续使用。

`MethodRegistry` 是运行时数据，可以来自 generated defaults，也可以在运行时扩展。可选 `axtp_json_rpc` 通过 `method_registry_json.hpp` 提供 JSON 文件加载。

Generated typed traits 和 schema codecs 只是可选便利头，不包含在 `<axtp.hpp>` 中，也不能成为 core routing 的前提。

## 当前边界

- `AxtpCore` 只处理 `ControlPayload`、`RpcPayload` 和 `StreamPayload`。
- `AxtpCore` 不知道 `ITransport`、concrete transport 或 `BasicBroker<>`。
- `BasicBroker<>` 不知道 `AxtpCore`，也不会回调 core。
- `AxtpEndpoint` 负责 `attachTransport()`、`poll()` 和 `flushOutbound()`。
- `AxtpCore` 不直接解析业务 JSON；JSON-RPC wire 解析属于 adapter/decoder。
- `AxtpCore` 不调用 `SchemaCodec`、`MethodTraits` 或业务 request/response struct。
- `AxtpCore` 不知道 legacy command ID 或 AXDP framing。
- TCP/WebSocket/HID 类是 `axtp-cpp-runtime/transports` 下的可选 target，不属于 `axtp_core`。

## 实现模式

- 用 `AxtpEndpoint` 连接 transport、core 和 broker。
- 用 core port adapter 和队列隔离 processor 内部状态。
- 用 `CoreEvent -> BrokerTask -> BrokerResult` 做业务分发。
- Transport 停留在 report/socket/message 边界，不能解析 payload 语义。
- Typed generated API 必须位于 raw/dynamic RPC 之上。
- 新 wire parsing 先实现 decoder/encoder，再接入 core pipeline。

完整模式和扩展 recipe 见 `docs/AXTP_CPP_RUNTIME_PATTERNS.md`。
