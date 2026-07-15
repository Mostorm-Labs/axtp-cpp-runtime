# cpp-runtime Layer Boundaries

Read this document before changing runtime code. The dependency direction is:

```text
NearCast product host -> Axent protocol bus -> axtp-cpp-runtime
```

## Runtime owns

- AXTP wire models, frame/payload codecs and protocol sessions.
- Locked/generated schema facts, IDs, registries and error values.
- RPC/broker/endpoint mechanics, SDK helpers and `ITransport` contracts.
- Protocol-level JSON-RPC helpers and conformance support.

## Runtime does not own

- TCP, WebSocket or HID concrete providers and their platform dependencies.
- Device/media/firmware Host profiles, product retry policy or UI behavior.
- Axent contracts, adapters, leases, firmware services or `axtpctl`.
- NearCast control routes, casting policy, AirPlay, decoding or rendering.

Runtime must never depend on Axent or NearCast. New protocol facts come from the
locked AXTP specification and generated artifacts; do not invent them in SDK or
runtime implementation code. The only repository-owned third-party runtime
dependency is the pinned JSON library.

## Placement rule

Put protocol mechanics here, reusable device and transport integration in
Axent, and product behavior in the product host. If a change needs an upper
layer type or policy decision, it does not belong in cpp-runtime.
