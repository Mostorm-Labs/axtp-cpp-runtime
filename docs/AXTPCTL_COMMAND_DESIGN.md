# axtpctl 命令设计

## 概要

`axtp-cpp-runtime/tools/axtpctl` 是 AXTP 命令行工具，用于调试、产测和集成检查。它依赖 `axtp-cpp-runtime/sdk`，除明确的 inspect 命令外，不应直接操作低层 frame decoder。

Runtime 调用应走 SDK；SDK 再通过 `AxtpEndpoint` 连接 `ITransport`、`AxtpCore` 和 `BasicBroker<>`。

P0 实现一组小而可用的 dynamic RPC 命令：

- `--help`
- `call <method|--method-id>`，支持 JSON/TLV/Raw body
- `capability methods`
- `list-methods`
- `ping`
- `handshake`
- `list-hid`
- `read-hid`
- `inspect frame --hex <HEX>`

P1 保留更大的规划面：event watch/emit、stream read/write、mock-server 和完整 test-vector runner。

## 全局语法

```bash
axtpctl \
  --transport tcp|ws|hid|ble|uart|mock \
  --endpoint <endpoint> \
  --wire framed-binary|websocket-json-rpc \
  --encoding json|tlv|raw \
  --registry-file FILE \
  --timeout 5000 \
  --verbose \
  <command>
```

`mock` 是 P0 smoke test 的默认 transport。协议级调试优先走 `mock` 或 TCP/WebSocket mock server。HID 只面向真实设备调试，不提供 HID mock/socket simulation。真实设备应通过 SDK transport connector 或可选 transport factory 接入。HID/TCP/WebSocket concrete dependency 属于 tool/runtime dependency，不得下沉到 cpp/core。

## P0 命令

### call

```bash
axtpctl --transport mock call audio.getAlgorithmConfig --json '{}'
axtpctl --transport mock call audio.setAlgorithmConfig --json '{"noiseSuppression":{"enabled":true,"level":3}}'
axtpctl --transport mock call --method-id 0x90010001 --raw-hex cafe
axtpctl --transport mock --registry-file ./methods.json call vendor.echo --json '{"value":80}'
axtpctl -t hid --path '<path from list-hid>' call audio.getAlgorithmConfig --json '{}'
```

行为：

- 通过 runtime `MethodRegistry` 解析 method name。
- 支持 `--json`、`--tlv-hex`、`--tlv-file`、`--raw-hex` 和 `--raw-file`。
- 使用 SDK dynamic API，不要求 generated typed C++ wrapper。
- JSON response body 默认按 JSON 输出；TLV/Raw 可按 hex 输出或写入文件。

### capability methods

```bash
axtpctl capability methods
```

把 generated method registry 打印为 JSON。

### ping

```bash
axtpctl --transport mock ping
```

P0 mock ping 返回本地 success JSON 文档。真实 transport ping 等 SDK 拥有 client-side transport connection management 后，再接到 CONTROL/RPC。

### handshake

```bash
axtpctl handshake -t hid --path '<path from list-hid>'
axtpctl handshake -t hid
axtpctl handshake -t tcp --host 127.0.0.1 --port 9000
axtpctl handshake -t websocket --host 127.0.0.1 --port 9001
```

`handshake` 是 transport-generic app-ready/session 命令。它使用统一 transport factory 和 SDK `ensureAppReady()`，不属于 HID 专用命令。

### list-hid / read-hid

```bash
axtpctl -t hid list-hid
axtpctl -t hid read-hid --timeout 10000
axtpctl -t hid read-hid --path '<path from list-hid>' --timeout 10000
axtpctl -t hid read-hid --usage-page 0 --timeout 10000
```

这两个命令只用于真实 HID 设备诊断。HID mock/socket simulation 不再提供；协议调试走 `mock` 或 TCP/WebSocket mock server。
CLI 默认 HID 目标为 VID/PID `0x0581:0x2581`、usagePage `0x81`；需要调试其他设备或 interface 时再显式传 `--vid`/`--pid`/`--usage-page`。
当同一个 VID/PID 暴露多个 HID interface 时，可通过 `--usage-page`/`--usage` 过滤并解析到具体设备 path。

### inspect frame

```bash
axtpctl inspect frame --hex "415801020000000000000001..."
```

解析 hex AXTP standard frame，打印 header fields、payload length、payload type 和 CRC 信息。这个命令可以直接使用 core model constants，因为它是明确的诊断工具。

## P1 命令

以下命令保留在设计中，但不是 P0 必须实现：

```bash
axtpctl event watch <event|--all>
axtpctl event emit <event> --data JSON
axtpctl capability get
axtpctl capability events
axtpctl stream open <stream>
axtpctl stream read <stream> --output FILE
axtpctl stream write <stream> --input FILE
axtpctl test-vector run PATH
axtpctl mock-server --listen HOST:PORT
```

这些命令后续仍应使用 SDK-level API，不应直接伸进 cpp/core 内部。

## 内部执行流程

普通命令执行路径：

```text
argv
  -> parse global options
  -> load generated MethodRegistry
  -> optionally merge --registry-file
  -> choose mock or configured concrete transport
  -> construct AxtpClient
  -> execute SDK dynamic API
  -> render json/hex/file output
```

只有 `inspect` 命令可以直接接触 frame/payload decoder。面向用户的设备操作应通过 SDK call，以保证 CLI 行为和应用行为一致。

## 命令分发规则

- Global options 先于 command-specific options 解析。
- `--command` 和 `-c` 是 `call <method>` 的 shortcut。
- `--params` 只作为 `--json` 的兼容 alias。
- `--json-file`、`--tlv-file`、`--raw-file` 必须和对应 inline 参数互斥。
- `--registry-file` 用于扩展 runtime method lookup，不应要求 typed generated wrapper。
- 输出默认规则：JSON payload 输出 JSON；TLV/Raw 输出 hex，除非 `--output` 指定文件。

## Transport 策略

`axtpctl` 可以链接 optional transport target，因为它是 tool target。它不能把 concrete transport dependency 下沉到 cpp/core。命令实现应把 transport factory 逻辑保持在 CLI/SDK 边界：

```text
mock      -> testing/mock transport or local handler
hid       -> optional axtp_transport_hidapi
tcp       -> built-in axtp_transport_tcp_native
ws        -> optional axtp_transport_websocket_ix
ble/uart  -> reserved endpoint values until concrete transports exist
```

仓库只构建一个 `axtpctl` CLI。HID 真实设备诊断通过 `axtpctl -t hid ...`
进入；MediaHost 等业务应用复用内部 `axtp_toolkit`，但不作为 `axtpctl` 子命令。

## 文档链接

- Runtime patterns: `docs/AXTP_CPP_RUNTIME_PATTERNS.md`
- Execution flow: `docs/AXTP_CPP_EXECUTION_FLOW.md`
- Core API: `docs/AXTP_CORE_API_DESIGN.md`
- SDK API: `docs/AXTP_SDK_API_DESIGN.md`
- C++ style: `docs/AXTP_CPP_STYLE.md`
