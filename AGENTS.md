# AGENTS.md

本仓库是 AXTP C++ runtime、SDK、optional transports 和 `axtpctl` 工具仓库。协议语义来自主规范仓库 `axtp/`，本仓库只实现和生成 C++ 侧产物。

## 先读

- `README.md`
- `AXTP_SPEC.lock.yaml`
- `devtools/generators/README.md`
- `docs/AXTP_CPP_RUNTIME_PATTERNS.md`
- `docs/AXTP_CPP_EXECUTION_FLOW.md`
- `docs/AXTP_CORE_API_DESIGN.md`
- `docs/AXTP_SDK_API_DESIGN.md`
- `docs/AXTPCTL_COMMAND_DESIGN.md`
- `docs/AXTP_CPP_STYLE.md`

需要判断协议事实时，回到主规范仓库读取 `axtp/AGENTS.md`、`protocol/axtp.protocol.yaml`、`docs/generated/**` 和 `docs/conformance/**`。

## Spec Lock 规则

- 不依赖 AXTP `main`。本仓库必须绑定 `AXTP_SPEC.lock.yaml` 里的 tag/commit。
- 本地生成或 conformance 运行前设置 `AXTP_SPEC_PATH=/path/to/axtp`，该 checkout 应匹配 lock。
- 也可使用 `third_party/axtp-spec`，但必须 checkout 到 lock 记录的 tag 或 commit。
- 升级 spec 使用 `devtools/scripts/upgrade-axtp-spec.sh spec/vX.Y.Z`，然后运行 lock、生成、版本、CMake/CTest 和 conformance 检查。

## 生成物边界

本仓库自己的 generator 位于 `devtools/generators/`，消费主 spec，输出 C++ runtime 产物。

不要手写：

- `include/core/protocol/generated/**`
- `generated/axtp_generated_manifest.json`

刷新方式：

```bash
pnpm --dir devtools/generators build
pnpm --dir devtools/generators test
AXTP_SPEC_PATH=/path/to/axtp pnpm --dir devtools/generators generate:runtime
devtools/scripts/check-generated-version.sh
```

`devtools/scripts/generate-axtp-artifacts.sh` 会调用 C++ emitter 写 `include/core/protocol/generated/` 并更新 generated manifest。

## Runtime 实现边界

- 可以修改 `include/`、`src/`、`tools/`、`cmake/targets/` 和 `tests/` 中的手写源码。
- 不在 runtime、SDK 或 tool 里发明新的 method/event/schema/error/capability 语义；缺失协议事实要回主规范仓库走 draft/adopt/amend/generate。
- `PayloadType` 只选择 parser，不承载业务 domain 语义。不要把 VIDEO、FIRMWARE、FILE 等业务概念塞进 frame/payload 层。
- 区分 `MessageId`、RPC `requestId` 和 STREAM `streamId`。多字节整数按 AXTP big-endian 规则处理。

## 常用验证

```bash
cmake -S . -B build/root
cmake --build build/root
ctest --test-dir build/root --output-on-failure

devtools/scripts/check-axtp-spec-lock.sh
devtools/scripts/check-generated-version.sh
AXTP_SPEC_PATH=/path/to/axtp devtools/scripts/run-conformance.sh
```

按改动范围选择最小足够的验证；改 generated/spec lock 时要跑版本和 conformance 相关检查。
