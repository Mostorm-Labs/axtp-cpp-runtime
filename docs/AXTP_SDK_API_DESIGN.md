# AXTP SDK API 设计

## 概要

`axtp-cpp-runtime/sdk` 是面向应用的 C++ 层，构建在 `axtp-cpp-runtime/core` 之上。它隐藏 Frame、Message、Payload 等底层细节，对外提供 client/server、endpoint、call、event、capability 和 typed method helper。

SDK 直接复用 runtime 分层：

```text
ITransport <-> AxtpEndpoint -> AxtpCore -> BasicBroker
```

Client 和 server wrapper 拥有 `BasicBroker<>` 与 `AxtpEndpoint`，但不会把 transport 直接接进 `AxtpCore`。

SDK 以 dynamic RPC 为默认路径，支持三个 API level：

| Level | 形态 | 用途 |
|---|---|---|
| Raw API | methodId + encoding + body bytes | 调试、vendor 私有方法、桥接 |
| Dynamic API | methodName + JSON/TLV body | 默认面向扩展的应用 API |
| Typed API | generated request/response structs | 稳定 domain 的可选便利层 |

SDK P0 目标是跑通一条可构建、可测试的纵向链路：

- 构造 client 或 server。
- 绑定 transport，或使用本地 mock handler。
- 通过 `callJson`、`callTlv`、`callRaw` 按 ID 或 name 调用方法。
- Typed call 只作为 raw/dynamic call 的 wrapper。
- 为常用 device/display/capability 操作提供 generated-style domain client facade。

真实异步网络连接管理、完整 event subscription、stream helper 和完整 schema-aware codec 属于 P1/P2。

相关实现文档：

| 文档 | 作用 |
|---|---|
| `docs/AXTP_CPP_EXECUTION_FLOW.md` | Client/server call flow、endpoint polling 和 CLI flow |
| `docs/AXTP_CPP_RUNTIME_PATTERNS.md` | Dynamic RPC、endpoint glue、transport boundary 和 extension recipe |
| `docs/AXTP_CPP_STYLE.md` | C++ 命名、include、格式化和 ownership 规则 |

## 包布局

```text
axtp-cpp-runtime/
  include/
    axtp_sdk.hpp
    sdk/
      axtp_client.hpp
      axtp_server.hpp
      axtp_device.hpp
      client_options.hpp
      endpoints.hpp
      call_options.hpp
      event_subscription.hpp
      capability_client.hpp
      stream_client.hpp
      sdk_error.hpp
      sdk_result.hpp
      generated/
        audio_client.h
      axtp_sdk_all.hpp
  sdk/
  tests/
    sdk/
```

SDK namespace 是 `axtp::sdk`。

## Client API

P0 client 入口：

```cpp
class AxtpClient {
public:
    explicit AxtpClient(ClientOptions options = {});

    void attachTransport(std::unique_ptr<ITransport> transport);
    void connect(const TcpEndpoint& endpoint);
    void connect(const WebSocketEndpoint& endpoint);
    void connect(const HidEndpoint& endpoint);
    void connect(const BleEndpoint& endpoint);
    void connect(const UartEndpoint& endpoint);
    void close();
    bool isConnected() const;
    const SdkError& lastError() const;
    void poll();

    MethodRegistry& registry();
    const MethodRegistry& registry() const;

    RpcPayload callRaw(RpcPayload request, CallOptions options = {});
    Bytes callRaw(std::uint32_t methodId, RpcEncoding encoding, Bytes body, CallOptions options = {});
    std::string callJson(std::string_view methodName, std::string_view paramsJson, CallOptions options = {});
    std::string callJson(std::uint32_t methodId, std::string_view paramsJson, CallOptions options = {});
    Bytes callTlv(std::string_view methodName, Bytes tlvBody, CallOptions options = {});
    Bytes callTlv(std::uint32_t methodId, Bytes tlvBody, CallOptions options = {});
    Bytes callRawBytes(std::uint32_t methodId, Bytes body, CallOptions options = {});

    template <MethodId Id>
    typename MethodTraits<Id>::Response callTyped(
        const typename MethodTraits<Id>::Request& request,
        CallOptions options = {});
};
```

`callJson(methodName, paramsJson)` 通过 `MethodRegistry` 解析 method name，只把业务 params object 放进 `RpcPayload.body`，不要求 generated C++ request type。

Typed call 是便利 wrapper：typed request -> schema codec -> `callRaw` -> schema codec -> typed response。

## Server API

P0 server 入口：

```cpp
class AxtpServer {
public:
    void attachTransport(std::unique_ptr<ITransport> transport);
    void close();
    void poll();

    void onRaw(std::uint32_t methodId, std::function<Bytes(const RpcPayload&)> handler);
    void onJson(std::string_view methodName, JsonRpcHandler handler);
    void onTlv(std::string_view methodName, TlvRpcHandler handler);
    void emitRaw(RpcPayload payload);
};
```

Server API 是 `BasicBroker<>` 和 `AxtpEndpoint` 之上的薄封装。它应优先提供 `onJson`、`onTlv`、`onRaw`；typed handler 是可选 generator 输出。完整多连接 routing 不属于 P0。

## Domain Client

Generated-style facade 让常用 domain 调用更容易发现：

```cpp
AxtpClient client;
AxtpDevice device(client);

auto config = device.audio.getAlgorithmConfig();
device.audio.setAlgorithmConfig(request);
```

这些 facade 是 P0 手写占位，匹配当前已采纳的协议面。后续 generator 应从 protocol registry 生成完整集合；但新增业务方法不应强迫 SDK 重新编译，只要 caller 能提供 method name/id 加 JSON/TLV/Raw body，就应该能调用。

## Endpoint 与 Options

Endpoint 是 value type：

- `TcpEndpoint { host, port }`
- `WebSocketEndpoint { url, wireMode }`
- `HidEndpoint { vendorId, productId, serialNumber }`
- `BleEndpoint { deviceId, serviceUuid, characteristicUuid }`
- `UartEndpoint { path, baudRate }`

P0 对 transport 创建能力刻意保持有限，因为当前 cpp/core TCP/WebSocket 类偏 server-oriented test transport。生产 client connector 要等 core transport abstraction 增加 client connection API 后再落地。

## 执行流程

`AxtpClient::callJson()` 和其他 dynamic call 走这条路径：

```text
method name/id + params/body
  -> MethodRegistry lookup
  -> RpcPayload
  -> AxtpEndpoint::sendRpcRequest()
  -> AxtpCore outbound encode
  -> ITransport::sendBytes()
  -> poll loop
  -> AxtpCore pending response table
  -> SDK result bytes/string/typed response
```

`AxtpServer::poll()` 走这条路径：

```text
transport bytes
  -> AxtpEndpoint
  -> AxtpCore
  -> CoreEvent
  -> BasicBroker<> handler
  -> BrokerResult
  -> AxtpCore response encode
  -> transport sendBytes()
```

SDK wrapper 必须保持这个方向。它可以拥有 endpoint、broker 和 transport object，但不能让 `AxtpCore` 反向知道 SDK abstraction。

## Connector 边界

`attachTransport(std::unique_ptr<ITransport>)` 是稳定 P0 路径。`TcpEndpoint`、`WebSocketEndpoint`、`HidEndpoint` 等 endpoint value type 保留为 API 占位，但真实平台对象构造属于可选 connector/helper code。

这样可以让 `axtp_sdk` 在 mock 或应用自持 transport 场景下可用，同时避免平台库变成 mandatory dependency。

## Dynamic RPC 策略

SDK public example 应优先使用 dynamic call：

```cpp
client.callJson("audio.getAlgorithmConfig", "{}");
client.callTlv("audio.setAlgorithmConfig", bytes);
client.callRawBytes(0x90010001, bytes);
```

Typed call 可用于稳定 generated domain，但必须保持为 dynamic/raw call 的 wrapper。新增 vendor 或 experimental method 应能通过 `MethodRegistry` 调用，不需要重新生成 SDK。

## Call Wait Hooks

`CallOptions::progress` lets an embedding scheduler submit work after each
`AxtpClient::poll()` while a synchronous RPC is waiting. The callback must not
recursively use the same client. After each poll, `callRaw()` runs `progress`,
checks matching and accepted fallback responses, then checks `cancelled` and
the deadline.

Cancellation returns `ErrorCode::Canceled`; timeout continues to return
`ErrorCode::RpcResponseTimeout`. Both outcomes abandon the request ID so a late
response is consumed and discarded instead of satisfying a later call.
Exceptions from either wait hook are contained and reported as
`ErrorCode::InternalError`.

The timeout is one absolute budget covering automatic control open,
identification, and the business response. Because `IDENTIFIED` has no request
ID, cancelling after `IDENTIFY` leaves that handshake outstanding; a later call
on the same physical transport resumes the original wait rather than sending a
second `IDENTIFY` that could be satisfied by the old response.

Non-zero request IDs are single-use until the physical transport is detached.
Successful, cancelled, and timed-out calls all retire their ID. Endpoint/Core
callers must check the boolean result from `sendRpcRequest()` /
`expectRpcResponse()` before sending. `acceptAnyResponse` is only a compatibility
path for a legacy ID-zero response, which cannot be correlated reliably after
cancellation; new peers must echo the non-zero request ID.
