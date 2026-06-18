# AXTP C++ Runtime Generator

This generator is owned by `axtp-cpp-runtime`.

It consumes the AXTP spec checkout selected by `AXTP_SPEC_PATH`, or
`third_party/axtp-spec` when the environment variable is unset. The checkout must
match `AXTP_SPEC.lock.yaml`.

```bash
export AXTP_SPEC_PATH=/path/to/axtp
pnpm --dir generators install
pnpm --dir generators build
pnpm --dir generators test
pnpm --dir generators generate:runtime
```

Generated C++ artifacts are written to `core/include/protocol/generated/`.
